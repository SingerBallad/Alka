#include <Alka/Core/Error.hpp>
#include <Alka/Detail/Compat.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Js/Value.hpp>
#include <Alka/Runtime/Lifetime.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace Alka {

namespace Detail {

namespace {

auto isolatesMutex() -> std::shared_mutex& {
    static auto& mutex = *new std::shared_mutex;
    return mutex;
}
std::atomic<std::uint64_t> gRegistryGeneration{0};

const char kInstanceTag = 0;

std::atomic<std::uint64_t> gContextGeneration{0};
struct ContextCache {
    PerContextData* mData = nullptr;
    std::uint64_t mGeneration = 0;
};
thread_local ContextCache tContextCache;

auto isolateRegistry() -> std::unordered_map<v8::Isolate*, std::unique_ptr<PerIsolateData>>& {
    static auto& registry = *new std::unordered_map<v8::Isolate*, std::unique_ptr<PerIsolateData>>;
    return registry;
}

struct IsolateCache {
    v8::Isolate* mIsolate = nullptr;
    PerIsolateData* mData = nullptr;
    std::uint64_t mGeneration = 0;
};
thread_local IsolateCache tIsolateCache;

thread_local int tInWeakCallback = 0;

struct WeakCallbackScope {
    WeakCallbackScope() { ++tInWeakCallback; }
    WeakCallbackScope(const WeakCallbackScope&) = delete;
    WeakCallbackScope& operator=(const WeakCallbackScope&) = delete;
    ~WeakCallbackScope() { --tInWeakCallback; }
};

void releaseHolder(PerIsolateData& data, std::unique_ptr<Holder> holder) {
    if (!holder) {
        return;
    }
    if (holder->mExternalBytes != 0) {
        adjustExternalMemory(data, -holder->mExternalBytes);
        holder->mExternalBytes = 0;
    }
    holder->mEntry = nullptr;
    if (data.mDestructionMode == DestructionMode::Deferred) {
        data.mDeferred.push_back(std::move(holder));
    }
}

void secondPassCallback(const v8::WeakCallbackInfo<WrapperEntry>& info) {
    const WeakCallbackScope inCallback;
    const auto entry = info.GetParameter();
    if (entry->mData == nullptr) {
        delete entry;
        return;
    }
    entry->mData->mFinalizing.remove(entry);
    releaseHolder(*entry->mData, std::move(entry->mHolder));
    delete entry;
}

void firstPassCallback(const v8::WeakCallbackInfo<WrapperEntry>& info);

struct PinToken {
    std::shared_ptr<void> mOwner;
    std::shared_ptr<IsolateLink> mLink;
    std::shared_ptr<WrapperEntry*> mEntry; // nulled if the entry dies first

    PinToken(std::shared_ptr<void> owner, std::shared_ptr<IsolateLink> link,
             std::shared_ptr<WrapperEntry*> entry)
        : mOwner(std::move(owner)), mLink(std::move(link)), mEntry(std::move(entry)) {}
    PinToken(const PinToken&) = delete;
    PinToken& operator=(const PinToken&) = delete;
    ~PinToken() {
        if (mLink->canReleaseHere() && tInWeakCallback == 0 && mLink->alive()) {
            unpin(mEntry);
            return;
        }
        (void)mLink->post([entry = mEntry] { unpin(entry); });
    }

    static void unpin(const std::shared_ptr<WrapperEntry*>& ref) {
        WrapperEntry* const entry = *ref;
        if (entry != nullptr && entry->mHolder && entry->mHolder->mPin.expired() &&
            !entry->mHandle.IsEmpty()) {
            entry->mHandle.SetWeak(entry, &firstPassCallback, v8::WeakCallbackType::kParameter);
        }
    }
};

void firstPassCallback(const v8::WeakCallbackInfo<WrapperEntry>& info) {
    const WeakCallbackScope inCallback;
    const auto entry = info.GetParameter();
    entry->mHandle.Reset();
    entry->mData->mLiveWrappers.remove(entry);
    entry->mData->mFinalizing.push(entry);
    if (entry->mCache != nullptr) {
        entry->mCache->mWrapperCache.erase(entry->mKey, entry);
    }
    info.SetSecondPassCallback(&secondPassCallback);
}

void contextGoneSecondPass(const v8::WeakCallbackInfo<PerContextData>& info) {
    const auto pcd = info.GetParameter();
    if (pcd->mIsolateData == nullptr) {
        delete pcd; // orphaned by disposeIsolate
        return;
    }
    gContextGeneration.fetch_add(1, std::memory_order_acq_rel);
    std::erase_if(pcd->mIsolateData->mContexts,
                  [&](const auto& owned) { return owned.get() == pcd; });
}

void contextGoneCallback(const v8::WeakCallbackInfo<PerContextData>& info) {
    const auto pcd = info.GetParameter();
    pcd->mContext.Reset();
    pcd->mDying = true;
    pcd->mIsolateData->mLiveWrappers.forEach([&](WrapperEntry* entry) {
        if (entry->mCache == pcd) {
            entry->mCache = nullptr;
        }
    });
    info.SetSecondPassCallback(&contextGoneSecondPass);
}

auto typeIndexMutex() -> std::shared_mutex& {
    static auto& mutex = *new std::shared_mutex; // (never destroyed)
    return mutex;
}
auto typeIndexRegistry() -> std::unordered_map<std::type_index, KnownType>& {
    static auto& registry =
        *new std::unordered_map<std::type_index, KnownType>; // (never destroyed)
    return registry;
}

bool nodeOnChain(const TypeNode* from, const TypeNode* target) {
    for (auto n = from; n != nullptr; n = n->mParent) {
        if (n == target) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] int ownershipRank(const Ownership ownership) {
    switch (ownership) {
    case Ownership::Borrowed:
        return 0;
    case Ownership::Weak:
        return 1;
    case Ownership::Owned:
        return 2;
    }
    return 0;
}

void evict(PerContextData& pcd, WrapperEntry* entry) {
    pcd.mWrapperCache.erase(entry->mKey, entry);
    entry->mCache = nullptr;
}

auto findCachedEntry(PerContextData& pcd, const TypeNode* node, const void* key) -> WrapperEntry* {
    const auto entry = pcd.mWrapperCache.find(WrapperKey{.mAddress = key, .mRoot = rootOf(node)});
    if (entry == nullptr) {
        return nullptr;
    }
    if (!nodeOnChain(entry->mNode, node) && !nodeOnChain(node, entry->mNode)) {
        return nullptr;
    }
    if (!entry->mHolder->alive()) {
        evict(pcd, entry);
        return nullptr;
    }
    return entry;
}

auto newEntry(v8::Isolate* isolate, PerIsolateData& data, const v8::Local<v8::Object> object,
              std::unique_ptr<Holder> holder) -> WrapperEntry* {
    auto* entry = new WrapperEntry;
    holder->mEntry = entry;
    entry->mHolder = std::move(holder);
    entry->mData = &data;
    entry->mHandle.Reset(isolate, object);
    entry->mHandle.SetWeak(entry, &firstPassCallback, v8::WeakCallbackType::kParameter);
    data.mLiveWrappers.push(entry);
    return entry;
}

template <class T, class Make>
auto hotHandle(v8::Isolate* isolate, std::vector<v8::Global<T>>& table, const std::size_t slot,
               Make&& make) -> v8::Local<T> {
    if (slot >= table.size()) {
        table.resize(slot + 1);
    }
    auto& handle = table[slot];
    if (!handle.IsEmpty()) {
        return handle.Get(isolate);
    }
    const v8::Local<T> fresh = std::forward<Make>(make)();
    handle.Reset(isolate, fresh);
    return fresh;
}

std::atomic<std::size_t> gNextHotKeySlot{0};

} // namespace

auto newHotKeySlot() noexcept -> std::size_t {
    return gNextHotKeySlot.fetch_add(1, std::memory_order_relaxed);
}

auto hotKey(v8::Isolate* isolate, const std::size_t slot, const std::string_view text)
    -> v8::Local<v8::String> {
    return hotHandle(isolate, perIsolateData(isolate).mHotKeys, slot,
                     [&] { return v8Str(isolate, text, true); });
}

auto hotPrivate(v8::Isolate* isolate, const std::size_t slot, const std::string_view name)
    -> v8::Local<v8::Private> {
    return hotHandle(isolate, perIsolateData(isolate).mHotPrivates, slot,
                     [&] { return privateSymbol(isolate, name); });
}

auto wellKnownSymbol(v8::Isolate* isolate, const std::string_view name) -> v8::Local<v8::Symbol> {
    auto& known = perIsolateData(isolate).mWellKnownSymbols;
    if (const auto it = known.find(name); it != known.end()) {
        return it->second.Get(isolate); // (empty: this V8 lacks it)
    }
    v8::EscapableHandleScope scope(isolate);
    const auto context = v8::Context::New(isolate);
    const v8::Context::Scope contextScope(context);
    v8::Local<v8::Symbol> found;
    v8::Local<v8::Value> constructor;
    v8::Local<v8::Value> value;
    if (context->Global()->Get(context, v8Str(isolate, "Symbol", true)).ToLocal(&constructor) &&
        constructor->IsFunction() &&
        constructor.As<v8::Object>()->Get(context, v8Str(isolate, name, true)).ToLocal(&value) &&
        value->IsSymbol()) {
        found = value.As<v8::Symbol>();
    }
    auto& slot = known[std::string(name)];
    if (found.IsEmpty()) {
        return {};
    }
    slot.Reset(isolate, found);
    return scope.Escape(found);
}

auto perIsolateData(v8::Isolate* isolate) -> PerIsolateData& {
    const auto generation = gRegistryGeneration.load(std::memory_order_acquire);
    if (auto& cache = tIsolateCache; cache.mIsolate == isolate && cache.mGeneration == generation) {
        return *cache.mData;
    }
    PerIsolateData* data = nullptr;
    {
        const std::shared_lock lock(isolatesMutex());
        if (const auto it = isolateRegistry().find(isolate); it != isolateRegistry().end()) {
            data = it->second.get();
        }
    }
    if (data == nullptr) {
        const std::unique_lock lock(isolatesMutex());
        auto& slot = isolateRegistry()[isolate];
        if (!slot) {
            slot = std::make_unique<PerIsolateData>();
            slot->mIsolate = isolate;
            slot->mLink = std::make_shared<IsolateLink>(isolate);
        }
        data = slot.get();
    }
    tIsolateCache = IsolateCache{isolate, data, generation};
    return *data;
}

auto isolateLink(v8::Isolate* isolate) -> const std::shared_ptr<IsolateLink>& {
    return perIsolateData(isolate).mLink;
}

namespace {

auto findContextData(const v8::Local<v8::Context> context) -> PerContextData& {
    const auto isolate = v8::Isolate::GetCurrent();
    const auto fields = static_cast<int>(context->GetNumberOfEmbedderDataFields());
    int free = -1;
    for (int probe = 0; probe < ALKA_CONTEXT_EMBEDDER_PROBES; ++probe) {
        const int slot = ALKA_CONTEXT_EMBEDDER_SLOT + probe;
        if (slot >= fields) {
            free = free < 0 ? slot : free;
            break;
        }
        const auto held = contextSlotValue(context, slot);
        if (!held.IsEmpty() && held->IsUndefined()) {
            free = free < 0 ? slot : free;
            break;
        }
        if (held.IsEmpty() || !held->IsNumber()) {
            continue;
        }
        const auto* pointer = static_cast<PerContextData*>(
            contextPointer(isolate, context, slot, ALKA_EMBEDDER_TAG_CONTEXT));
        if (pointer == nullptr) {
            free = free < 0 ? slot : free;
            continue;
        }
        if (pointer->mInstance == &kInstanceTag) {
            return *const_cast<PerContextData*>(pointer);
        }
    }
    if (free < 0) {
        std::fprintf(stderr,
                     "Alka: context embedder-data slots %d..%d are all taken by the host; "
                     "build Alka with ALKA_CONTEXT_EMBEDDER_SLOT set to a free range\n",
                     ALKA_CONTEXT_EMBEDDER_SLOT,
                     ALKA_CONTEXT_EMBEDDER_SLOT + ALKA_CONTEXT_EMBEDDER_PROBES - 1);
        std::abort();
    }
    auto& data = perIsolateData(isolate);
    auto owned = std::make_unique<PerContextData>();
    const auto pcd = owned.get();
    pcd->mInstance = &kInstanceTag;
    pcd->mSlot = free;
    pcd->mIsolateData = &data;
    pcd->mContext.Reset(isolate, context);
    pcd->mContext.SetWeak(pcd, &contextGoneCallback, v8::WeakCallbackType::kParameter);
    data.mContexts.push_back(std::move(owned));
    setContextPointer(context, free, pcd, ALKA_EMBEDDER_TAG_CONTEXT);
    return *pcd;
}

} // namespace

auto perContextData(const v8::Local<v8::Context> context) -> PerContextData& {
    const auto generation = gContextGeneration.load(std::memory_order_acquire);
    if (auto* const cached = tContextCache.mData; cached != nullptr &&
                                                  tContextCache.mGeneration == generation &&
                                                  cached->mContext == context) {
        return *cached;
    }
    auto& data = findContextData(context);
    tContextCache = ContextCache{.mData = &data, .mGeneration = generation};
    return data;
}

void registerTypeNodeIndex(const std::type_info& id, const TypeNode* node,
                           void* (*fromComplete)(void*)) {
    const std::unique_lock lock(typeIndexMutex());
    typeIndexRegistry().emplace(std::type_index(id),
                                KnownType{.mNode = node, .mFromComplete = fromComplete});
}

bool isKnownTypeNode(const TypeNode* node) {
    const std::shared_lock lock(typeIndexMutex());
    for (const auto& [mNode, mFromComplete] : typeIndexRegistry() | std::views::values) {
        if (mNode == node) {
            return true;
        }
    }
    return false;
}

auto lookupTypeNodeByTypeId(const std::type_info& id) -> KnownType {
    const std::shared_lock lock(typeIndexMutex());
    const auto& registry = typeIndexRegistry();
    const auto it = registry.find(std::type_index(id));
    return it != registry.end() ? it->second : KnownType{};
}

auto instantiateWrapper(ConvertCtx& cx, const TypeNode*& node, void*& ptr)
    -> v8::Local<v8::Object> {
    auto& data = perIsolateData(cx.mIsolate);
    for (auto current = node; current != nullptr; current = current->mParent) {
        if (const auto it = data.mClassTemplates.find(current); it != data.mClassTemplates.end()) {
            v8::Local<v8::Object> object;
            if (!it->second.Get(cx.mIsolate)
                     ->InstanceTemplate()
                     ->NewInstance(cx.mContext)
                     .ToLocal(&object)) {
                throw JsException("failed to instantiate wrapper for " +
                                  std::string(current->mName));
            }
            node = current;
            return object;
        }
        if (current->mUpcast != nullptr) {
            ptr = current->mUpcast(ptr);
        }
    }
    throw JsException("class " + std::string(node->mName) +
                      " (or any of its bases) is not installed in this isolate");
}

auto wrapNative(ConvertCtx& cx, const TypeNode* node, std::unique_ptr<Holder> holder,
                const void* cacheKey) -> v8::Local<v8::Object> {
    v8::EscapableHandleScope scope(cx.mIsolate);
    const auto object = instantiateWrapper(cx, node, holder->mPtr);
    attachWrapper(cx.mContext, object, node, std::move(holder), cacheKey);
    return scope.Escape(object);
}

auto findCachedWrapper(ConvertCtx& cx, const TypeNode* node, const void* key,
                       std::unique_ptr<Holder>* adopt) -> v8::Local<v8::Object> {
    auto& pcd = perContextData(cx.mContext);
    const auto entry = findCachedEntry(pcd, node, key);
    if (entry == nullptr) {
        return {};
    }
    const auto object = entry->mHandle.Get(cx.mIsolate);
    if (adopt != nullptr && *adopt &&
        ownershipRank((*adopt)->mOwnership) > ownershipRank(entry->mHolder->mOwnership)) {
        auto& holder = *entry->mHolder;
        auto previous = std::make_unique<Holder>(std::move(holder));
        holder = std::move(**adopt);
        adopt->reset();
        holder.mPtr = previous->mPtr;
        holder.mEntry = entry;
        if (holder.mExternalBytes != 0) {
            adjustExternalMemory(*entry->mData, holder.mExternalBytes);
        }
        releaseHolder(*entry->mData, std::move(previous));
        migrateRetained(cx, *entry, object);
    }
    return object;
}

void attachWrapper(const v8::Local<v8::Context> context, const v8::Local<v8::Object> target,
                   const TypeNode* node, std::unique_ptr<Holder> holder, const void* cacheKey) {
    const auto isolate = v8::Isolate::GetCurrent();
    writeInternals(target, node, holder.get());
    auto& data = perIsolateData(isolate);
    auto& pcd = perContextData(context);
    if (holder->mExternalBytes != 0) {
        adjustExternalMemory(data, holder->mExternalBytes);
    }
    const auto entry = newEntry(isolate, data, target, std::move(holder));
    entry->mNode = node;
    if (cacheKey != nullptr) {
        entry->mKey = WrapperKey{.mAddress = cacheKey, .mRoot = rootOf(node)};
        entry->mCache = &pcd;
        pcd.mWrapperCache.assign(entry->mKey, entry); // newest wrapper wins the identity slot
    }
    if (node->mAttachSelf != nullptr && entry->mHolder->mPtr != nullptr) {
        node->mAttachSelf(entry->mHolder->mPtr, isolate, context, target);
    }
}

void trackExternalHolder(v8::Isolate* isolate, const v8::Local<v8::Object> object,
                         std::unique_ptr<Holder> holder) {
    (void)newEntry(isolate, perIsolateData(isolate), object, std::move(holder));
}

void invalidateWrappers(v8::Isolate* isolate, const TypeNode* node, const void* key) {
    for (const auto& pcd : perIsolateData(isolate).mContexts) {
        const auto entry = findCachedEntry(*pcd, node, key);
        if (entry == nullptr || !entry->mHolder->borrowed()) {
            continue;
        }
        entry->mHolder->mPtr = nullptr;
        evict(*pcd, entry);
    }
}

auto findOwningHolder(v8::Isolate* isolate, const TypeNode* node, const void* key) -> Holder* {
    for (const auto& pcd : perIsolateData(isolate).mContexts) {
        if (const auto entry = findCachedEntry(*pcd, node, key);
            entry != nullptr && entry->mHolder->mOwnership == Ownership::Owned) {
            return entry->mHolder.get();
        }
    }
    return nullptr;
}

auto pinnedOwner(Holder& holder) -> std::shared_ptr<void> {
    if (holder.mParent != nullptr) {
        if (holder.isPath()) {
            return nullptr;
        }
        auto owner = pinnedOwner(const_cast<Holder&>(*holder.mParent));
        return owner ? std::shared_ptr<void>(std::move(owner), holder.mPtr) : nullptr;
    }
    switch (holder.mOwnership) {
    case Ownership::Borrowed:
        return nullptr;
    case Ownership::Weak:
        return holder.mWeak.lock();
    case Ownership::Owned:
        break;
    }
    if (auto existing = holder.mPin.lock()) {
        return existing;
    }
    const auto entry = holder.mEntry;
    if (entry == nullptr || entry->mHandle.IsEmpty()) {
        return holder.mOwner;
    }
    entry->mHandle.ClearWeak();
    if (!entry->mSelf) {
        entry->mSelf = std::make_shared<WrapperEntry*>(entry);
    }
    auto token = std::make_shared<PinToken>(holder.mOwner, entry->mData->mLink, entry->mSelf);
    std::shared_ptr<void> pin(std::move(token), holder.mOwner.get());
    holder.mPin = pin;
    return pin;
}

void releasePendingObjects() {
    while (tPendingReleases != nullptr && !tPendingReleases->empty()) {
        auto batch = std::move(*tPendingReleases);
        tPendingReleases->clear();
        batch.clear();
    }
}

void releaseWhenUnused(std::shared_ptr<void> owner) {
    if (!owner) {
        return;
    }
    if (tNativeDepth == 0) {
        owner.reset();
        return;
    }
    if (tPendingReleases == nullptr) {
        tPendingReleases = new std::vector<std::shared_ptr<void>>();
    }
    tPendingReleases->push_back(std::move(owner));
}

namespace {

void detachForDispose(PerIsolateData& data, WrapperEntry& entry) {
    Holder& holder = *entry.mHolder;
    holder.mPtr = nullptr;
    if (holder.mExternalBytes != 0) {
        adjustExternalMemory(data, -holder.mExternalBytes);
        holder.mExternalBytes = 0;
    }
    if (holder.mOwnership == Ownership::Owned) {
        releaseWhenUnused(std::move(holder.mOwner));
    }
    holder.mWeak.reset();
    if (entry.mCache != nullptr) {
        evict(*entry.mCache, &entry);
    }
}

} // namespace

void disposeFromJs(v8::Isolate* isolate, const v8::Local<v8::Object> self) {
    const Holder* const holder = isWrapper(self) ? readHolder(self) : nullptr;
    if (holder == nullptr || holder->mEntry == nullptr || holder->mPtr == nullptr) {
        return;
    }
    auto& data = perIsolateData(isolate);
    WrapperEntry* const entry = holder->mEntry;
    if (holder->isPath() || entry->mNode == nullptr) {
        detachForDispose(data, *entry);
        return;
    }
    const auto node = entry->mNode;
    const auto key = entry->mKey.mAddress;
    detachForDispose(data, *entry);
    for (const auto& pcd : data.mContexts) {
        if (const auto other = findCachedEntry(*pcd, node, key); other != nullptr) {
            detachForDispose(data, *other);
        }
    }
}

void adjustExternalMemory(PerIsolateData& data, const std::int64_t delta) {
    if (delta == 0) {
        return;
    }
    data.mExternalBytes += delta;
#if ALKA_HAS_EXTERNAL_MEMORY_ACCOUNTER
    if (delta > 0) {
        data.mAccounter.Increase(data.mIsolate, static_cast<std::size_t>(delta));
    } else {
        data.mAccounter.Decrease(data.mIsolate, static_cast<std::size_t>(-delta));
    }
#else
    data.mIsolate->AdjustAmountOfExternalAllocatedMemory(delta);
#endif
}

auto retainedStore(ConvertCtx& cx, const v8::Local<v8::Object> owner, const bool create)
    -> v8::Local<v8::Map> {
    const auto key = cachedPrivate<"Alka::retained">(cx.mIsolate);
    v8::Local<v8::Value> existing;
    if (owner->GetPrivate(cx.mContext, key).ToLocal(&existing) && existing->IsMap()) {
        return existing.As<v8::Map>();
    }
    if (!create) {
        return {};
    }
    const auto store = v8::Map::New(cx.mIsolate);
    (void)owner->SetPrivate(cx.mContext, key, store);
    return store;
}

auto nextRetainedSlot() -> std::uint32_t {
    static std::atomic<std::uint32_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

auto keepingWrapper(v8::Isolate* isolate, const v8::Local<v8::Object> owner)
    -> v8::Local<v8::Object> {
    if (owner.IsEmpty() || !isWrapper(owner)) {
        return owner;
    }
    const auto* holder = readHolder(owner);
    if (holder == nullptr || holder->mOwnership == Ownership::Owned || holder->mEntry == nullptr ||
        holder->mEntry->mNode == nullptr) {
        return owner;
    }
    const auto* entry = holder->mEntry;
    if (const auto* owning = findOwningHolder(isolate, entry->mNode, entry->mKey.mAddress);
        owning != nullptr && owning->mEntry != nullptr && !owning->mEntry->mHandle.IsEmpty()) {
        return owning->mEntry->mHandle.Get(isolate);
    }
    return owner;
}

void slotRetained(ConvertCtx& cx, const v8::Local<v8::Object> owner, RetainedCell& cell,
                  const v8::Local<v8::Value> value) {
    cell.mSlot = nextRetainedSlot();
    const auto store = retainedStore(cx, owner, true);
    std::ignore =
        store->Set(cx.mContext, v8::Integer::NewFromUnsigned(cx.mIsolate, cell.mSlot), value);
    cell.mValue = ValueState::make(cx.mIsolate, cx.mContext, value, true);
    cell.mOwner = ValueState::make(cx.mIsolate, cx.mContext, owner, true);
}

void trackStrongRetained(const v8::Local<v8::Object> owner,
                         const std::shared_ptr<RetainedCell>& cell) {
    if (owner.IsEmpty() || !isWrapper(owner)) {
        return;
    }
    const auto* holder = readHolder(owner);
    if (holder == nullptr || holder->mEntry == nullptr) {
        return;
    }
    auto& cells = holder->mEntry->mStrongRetained;
    std::erase_if(cells, [](const std::weak_ptr<RetainedCell>& c) { return c.expired(); });
    cells.push_back(cell);
}

void migrateRetained(ConvertCtx& cx, WrapperEntry& entry, const v8::Local<v8::Object> wrapper) {
    if (entry.mStrongRetained.empty() || !ownerKeepsValues(wrapper)) {
        return;
    }
    for (const auto& weakCell : std::exchange(entry.mStrongRetained, {})) {
        if (const auto cell = weakCell.lock(); cell != nullptr && cell->mSlot == 0 &&
                                               cell->mValue != nullptr && !cell->mValue->mDead &&
                                               !cell->mValue->mValue.IsEmpty()) {
            slotRetained(cx, wrapper, *cell, cell->mValue->mValue.Get(cx.mIsolate));
        }
    }
}

bool ownerKeepsValues(const v8::Local<v8::Object> owner) {
    if (owner.IsEmpty() || !isWrapper(owner)) {
        return false;
    }
    if (isGcWrapper(owner)) {
        return true;
    }
    const auto* holder = readHolder(owner);
    if (holder == nullptr || holder->mOwnership != Ownership::Owned || holder->mEntry == nullptr) {
        return false;
    }
    const auto shares = holder->mOwner.use_count() - (holder->mPin.expired() ? 0 : 1);
    return shares <= 1;
}

auto findPath(v8::Isolate* isolate, const WrapperEntry& owner, const PathKey& key)
    -> v8::Local<v8::Object> {
    if (!owner.mPaths) {
        return {};
    }
    const auto it = owner.mPaths->find(key);
    if (it == owner.mPaths->end() || it->second.IsEmpty()) {
        return {};
    }
    return it->second.Get(isolate);
}

void rememberPath(v8::Isolate* isolate, WrapperEntry& owner, const PathKey& key,
                  const v8::Local<v8::Object> view) {
    if (!owner.mPaths) {
        owner.mPaths =
            std::make_unique<std::unordered_map<PathKey, v8::Global<v8::Object>, PathKeyHash>>();
    }
    auto& paths = *owner.mPaths;
    if (paths.size() >= owner.mPathSweepAt) {
        std::erase_if(paths, [](const auto& path) { return path.second.IsEmpty(); });
        owner.mPathSweepAt = (std::max)(std::size_t{64}, paths.size() * 2);
    }
    auto& slot = paths[key];
    slot.Reset(isolate, view);
    slot.SetWeak();
}

void retainOwner(ConvertCtx& cx, const v8::Local<v8::Value> child,
                 const v8::Local<v8::Object> owner) {
    if (child.IsEmpty() || !child->IsObject() || child == owner) {
        return;
    }
    (void)child.As<v8::Object>()->SetPrivate(cx.mContext, cachedPrivate<"Alka::owner">(cx.mIsolate),
                                             owner);
}

void releaseKeptValues(ConvertCtx& cx, const v8::Local<v8::Object> self) {
    if (!self.IsEmpty()) {
        (void)self->DeletePrivate(cx.mContext, cachedPrivate<"Alka::keepAlive">(cx.mIsolate));
    }
}

void releaseKeptValues(v8::Isolate* isolate, const TypeNode* node, const void* key) {
    const v8::HandleScope scope(isolate);
    for (const auto& pcd : perIsolateData(isolate).mContexts) {
        const auto entry = findCachedEntry(*pcd, node, key);
        const auto context = pcd->mContext.Get(isolate);
        if (entry == nullptr || entry->mHandle.IsEmpty() || context.IsEmpty()) {
            continue;
        }
        ConvertCtx cx{isolate, context};
        releaseKeptValues(cx, entry->mHandle.Get(isolate));
    }
}

void keepAliveValue(ConvertCtx& cx, const v8::Local<v8::Object> self,
                    const v8::Local<v8::Value> value) {
    if (value.IsEmpty() || !value->IsObject()) {
        return;
    }
    const auto key = cachedPrivate<"Alka::keepAlive">(cx.mIsolate);
    v8::Local<v8::Value> existing;
    v8::Local<v8::Set> kept;
    if (self->GetPrivate(cx.mContext, key).ToLocal(&existing) && existing->IsSet()) {
        kept = existing.As<v8::Set>();
    } else {
        kept = v8::Set::New(cx.mIsolate);
        (void)self->SetPrivate(cx.mContext, key, kept);
    }
    std::ignore = kept->Add(cx.mContext, value);
}

void throwJsException(v8::Isolate* isolate, const std::string_view message,
                      const std::shared_ptr<ValueState>& original) {
    if (isolate->IsExecutionTerminating()) {
        return;
    }
    if (original && !original->mDead && original->mLink->isolate() == isolate &&
        !original->mValue.IsEmpty()) {
        isolate->ThrowException(original->mValue.Get(isolate));
        return;
    }
    throwError(isolate, message);
}

auto ensureNamespace(const v8::Local<v8::Context> context, const std::string_view dottedPath)
    -> v8::Local<v8::Object> {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::EscapableHandleScope scope(isolate);
    auto current = context->Global();

    auto rest = dottedPath;
    while (!rest.empty()) {
        const std::size_t dot = rest.find('.');
        const auto part = rest.substr(0, dot);
        rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
        if (part.empty()) {
            continue;
        }
        const auto key = v8Str(isolate, part, true);
        v8::Local<v8::Value> existing;
        if (current->GetRealNamedProperty(context, key).ToLocal(&existing) &&
            existing->IsObject()) {
            current = existing.As<v8::Object>();
        } else {
            const auto fresh = v8::Object::New(isolate);
            if (!current
                     ->DefineOwnProperty(
                         context, key, fresh,
                         static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete))
                     .FromMaybe(false)) {
                throw JsException("Alka: cannot install module '" + std::string(dottedPath) +
                                  "': '" + std::string(part) + "' is taken by something else");
            }
            current = fresh;
        }
    }
    return scope.Escape(current);
}

void defineNamespaceValue(const v8::Local<v8::Context> context, const v8::Local<v8::Object> target,
                          const std::string_view name, const v8::Local<v8::Value> value,
                          const bool isReadonly) {
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope scope(isolate);
    const auto attributes =
        isReadonly ? static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete) : v8::None;
    if (!target->DefineOwnProperty(context, v8Str(isolate, name, true), value, attributes)
             .FromMaybe(false)) {
        throw JsException("Alka: '" + std::string(name) +
                          "' is defined by two bindings installed in one namespace");
    }
}

} // namespace Detail

void setDestructionMode(v8::Isolate* isolate, const DestructionMode mode) {
    auto& data = Detail::perIsolateData(isolate);
    data.mDestructionMode = mode;
    if (mode == DestructionMode::Immediate) {
        data.mDeferred.clear();
    }
}

auto reportedExternalMemory(v8::Isolate* isolate) -> std::int64_t {
    return Detail::perIsolateData(isolate).mExternalBytes;
}

auto runDeferredDestruction(v8::Isolate* isolate, const std::size_t max) -> std::size_t {
    auto& data = Detail::perIsolateData(isolate);
    std::size_t destroyed = 0;
    while (destroyed < max && !data.mDeferred.empty()) {
        auto holder = std::move(data.mDeferred.back());
        data.mDeferred.pop_back();
        holder.reset();
        ++destroyed;
    }
    return destroyed;
}

void onDisposeIsolate(v8::Isolate* isolate, std::function<void()> hook) {
    Detail::perIsolateData(isolate).mDisposeHooks.push_back(std::move(hook));
}

void disposeIsolate(v8::Isolate* isolate) {
    while (true) {
        std::vector<std::function<void()>> hooks;
        {
            const std::unique_lock lock(Detail::isolatesMutex());
            const auto it = Detail::isolateRegistry().find(isolate);
            if (it == Detail::isolateRegistry().end() || it->second->mDisposeHooks.empty()) {
                break;
            }
            hooks = std::move(it->second->mDisposeHooks);
            it->second->mDisposeHooks.clear();
        }
        for (auto hook = hooks.rbegin(); hook != hooks.rend(); ++hook) {
            (*hook)();
        }
    }
    std::unique_ptr<Detail::PerIsolateData> data;
    {
        const std::unique_lock lock(Detail::isolatesMutex());
        auto& registry = Detail::isolateRegistry();
        if (const auto it = registry.find(isolate); it != registry.end()) {
            data = std::move(it->second);
            registry.erase(it);
        }
        Detail::gRegistryGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    if (!data) {
        return;
    }
    data->mLink->shutdown();
    const Detail::WeakCallbackScope tearingDown;
    data->mDestructionMode = DestructionMode::Immediate;
    data->mDeferred.clear();
    data->mLiveWrappers.forEach([&](Detail::WrapperEntry* entry) {
        entry->mHandle.Reset();
        Detail::releaseHolder(*data, std::move(entry->mHolder));
        delete entry;
    });
    data->mLiveWrappers.clear();
    data->mFinalizing.forEach([&](Detail::WrapperEntry* entry) {
        Detail::releaseHolder(*data, std::move(entry->mHolder));
        entry->mPaths.reset();
        entry->mData = nullptr;
        entry->mCache = nullptr;
    });
    data->mFinalizing.clear();
    {
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        for (auto& pcd : data->mContexts) {
            if (pcd->mDying) {
                pcd->mIsolateData = nullptr;
                std::ignore = pcd.release();
            } else if (!pcd->mContext.IsEmpty()) {
                Detail::setContextPointer(pcd->mContext.Get(isolate), pcd->mSlot, nullptr,
                                          ALKA_EMBEDDER_TAG_CONTEXT);
            }
        }
    }
    Detail::gContextGeneration.fetch_add(1, std::memory_order_acq_rel);
    data->mContexts.clear();
    data->mClassTemplates.clear();
    data->mHotKeys.clear();
    data->mHotPrivates.clear();
    Detail::adjustExternalMemory(*data, -data->mExternalBytes);
}

} // namespace Alka
