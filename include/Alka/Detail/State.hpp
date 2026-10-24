#pragma once

#include <Alka/Config.hpp>
#include <Alka/Convert/Converter.hpp>
#include <Alka/Core/FixedString.hpp>
#include <Alka/Detail/Compat.hpp>
#include <Alka/Detail/Holder.hpp>
#include <Alka/Detail/IsolateLink.hpp>
#include <Alka/Detail/TypeNode.hpp>

#include <v8.h>

#if __has_include(<v8-external-memory-accounter.h>)
#include <v8-external-memory-accounter.h>
#define ALKA_HAS_EXTERNAL_MEMORY_ACCOUNTER 1
#else
#define ALKA_HAS_EXTERNAL_MEMORY_ACCOUNTER 0
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Alka {

enum class DestructionMode : std::uint8_t {
    Immediate, // inside V8's GC callbacks
    Deferred,  // queued until Alka::runDeferredDestruction
};

} // namespace Alka

namespace Alka::Detail {

inline constexpr int kNodeField = 0;
inline constexpr int kHolderField = 1;
inline constexpr int kInternalFieldCount = 2;

[[nodiscard]] constexpr auto fieldTag(const int field) -> EmbedderTag {
    return field == kNodeField ? EmbedderTag{ALKA_EMBEDDER_TAG_TYPE_NODE}
                               : EmbedderTag{ALKA_EMBEDDER_TAG_HOLDER};
}

[[nodiscard]] inline auto readNode(const v8::Local<v8::Object> object) -> const TypeNode* {
    return static_cast<const TypeNode*>(internalPointer(object, kNodeField, fieldTag(kNodeField)));
}

[[nodiscard]] inline auto readHolder(const v8::Local<v8::Object> object) -> Holder* {
    if (const auto* node = readNode(object); node != nullptr && node->mGc) {
        return nullptr;
    }
    return static_cast<Holder*>(internalPointer(object, kHolderField, fieldTag(kHolderField)));
}

[[nodiscard]] inline auto readGcObject(const v8::Local<v8::Object> object) -> void* {
    return internalPointer(object, kHolderField, fieldTag(kHolderField));
}
inline void writeGcInternals(const v8::Local<v8::Object> object, const TypeNode* node, void* ptr) {
    setInternalPointer(object, kNodeField, const_cast<void*>(static_cast<const void*>(node)),
                       fieldTag(kNodeField));
    setInternalPointer(object, kHolderField, ptr, fieldTag(kHolderField));
}
[[nodiscard]] inline bool isGcWrapper(const v8::Local<v8::Object> object) {
    const auto* node = readNode(object);
    return node != nullptr && node->mGc;
}
inline void writeInternals(const v8::Local<v8::Object> object, const TypeNode* node,
                           Holder* holder) {
    setInternalPointer(object, kNodeField, const_cast<void*>(static_cast<const void*>(node)),
                       fieldTag(kNodeField));
    setInternalPointer(object, kHolderField, holder, fieldTag(kHolderField));
}
inline void clearInternal(const v8::Local<v8::Object> object, const int field) {
    setInternalPointer(object, field, nullptr, fieldTag(field));
}
[[nodiscard]] inline bool isWrapper(const v8::Local<v8::Object> object) {
    return object->InternalFieldCount() >= kInternalFieldCount;
}

[[nodiscard]] inline auto v8Str(v8::Isolate* isolate, const std::string_view text,
                                const bool internalized = false) -> v8::Local<v8::String> {
    return v8::String::NewFromUtf8(isolate, text.data(),
                                   internalized ? v8::NewStringType::kInternalized
                                                : v8::NewStringType::kNormal,
                                   static_cast<int>(text.size()))
        .ToLocalChecked();
}

[[nodiscard]] inline auto privateSymbol(v8::Isolate* isolate, const std::string_view name)
    -> v8::Local<v8::Private> {
    return v8::Private::ForApi(isolate, v8Str(isolate, name, true));
}

[[nodiscard]] auto newHotKeySlot() noexcept -> std::size_t;
[[nodiscard]] auto hotKey(v8::Isolate* isolate, std::size_t slot, std::string_view text)
    -> v8::Local<v8::String>;
[[nodiscard]] auto hotPrivate(v8::Isolate* isolate, std::size_t slot, std::string_view name)
    -> v8::Local<v8::Private>;

template <class Tag> [[nodiscard]] auto hotKeySlot() -> std::size_t {
    static const std::size_t slot = newHotKeySlot();
    return slot;
}

template <FixedString Text> struct HotName {};

template <FixedString Text>
[[nodiscard]] auto cachedKey(v8::Isolate* isolate) -> v8::Local<v8::String> {
    return hotKey(isolate, hotKeySlot<HotName<Text>>(), Text.view());
}

template <FixedString Name>
[[nodiscard]] auto cachedPrivate(v8::Isolate* isolate) -> v8::Local<v8::Private> {
    return hotPrivate(isolate, hotKeySlot<HotName<Name>>(), Name.view());
}

template <class D> [[nodiscard]] auto nameKey(v8::Isolate* isolate) -> v8::Local<v8::String> {
    return hotKey(isolate, hotKeySlot<D>(), D::name);
}

[[nodiscard]] constexpr bool isWellKnownSymbolName(const std::string_view name) {
    for (const std::string_view known :
         {"asyncDispose", "asyncIterator", "dispose", "hasInstance", "isConcatSpreadable",
          "iterator", "match", "matchAll", "replace", "search", "species", "split", "toPrimitive",
          "toStringTag", "unscopables"}) {
        if (name == known) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto wellKnownSymbol(v8::Isolate* isolate, std::string_view name)
    -> v8::Local<v8::Symbol>;

[[nodiscard]] inline auto memberKey(v8::Isolate* isolate, const std::string_view name)
    -> v8::Local<v8::Name> {
    if (!name.starts_with("@@")) {
        return v8Str(isolate, name, true);
    }
    const auto symbol = name.substr(2);
    if (!isWellKnownSymbolName(symbol)) {
        return v8::Symbol::For(isolate, v8Str(isolate, symbol));
    }
    if (symbol == "iterator") {
        return v8::Symbol::GetIterator(isolate);
    }
    if (symbol == "asyncIterator") {
        return v8::Symbol::GetAsyncIterator(isolate);
    }
    if (symbol == "toPrimitive") {
        return v8::Symbol::GetToPrimitive(isolate);
    }
    if (symbol == "toStringTag") {
        return v8::Symbol::GetToStringTag(isolate);
    }
    if (symbol == "hasInstance") {
        return v8::Symbol::GetHasInstance(isolate);
    }
    if (symbol == "isConcatSpreadable") {
        return v8::Symbol::GetIsConcatSpreadable(isolate);
    }
    if (symbol == "match") {
        return v8::Symbol::GetMatch(isolate);
    }
    if (symbol == "replace") {
        return v8::Symbol::GetReplace(isolate);
    }
    if (symbol == "search") {
        return v8::Symbol::GetSearch(isolate);
    }
    if (symbol == "split") {
        return v8::Symbol::GetSplit(isolate);
    }
    if (symbol == "unscopables") {
        return v8::Symbol::GetUnscopables(isolate);
    }
    return wellKnownSymbol(isolate, symbol);
}

[[nodiscard]] inline auto functionNameOf(const std::string_view name) -> std::string {
    if (!name.starts_with("@@")) {
        return std::string(name);
    }
    const auto symbol = name.substr(2);
    return isWellKnownSymbolName(symbol) ? "[Symbol." + std::string(symbol) + "]"
                                         : "[" + std::string(symbol) + "]";
}

inline void throwTypeError(v8::Isolate* isolate, const std::string_view message) {
    if (!isolate->IsExecutionTerminating()) {
        isolate->ThrowException(v8::Exception::TypeError(v8Str(isolate, message)));
    }
}

inline void throwError(v8::Isolate* isolate, const std::string_view message) {
    if (!isolate->IsExecutionTerminating()) {
        isolate->ThrowException(v8::Exception::Error(v8Str(isolate, message)));
    }
}

struct BaseCallMarker {
    const void* mObject = nullptr;
    std::string_view mName;
};

inline thread_local BaseCallMarker tBaseCall;

class BaseCallScope {
public:
    BaseCallScope(const void* object, const std::string_view name) : mSaved(tBaseCall) {
        tBaseCall = BaseCallMarker{.mObject = object, .mName = name};
    }
    BaseCallScope(const BaseCallScope&) = delete;
    BaseCallScope& operator=(const BaseCallScope&) = delete;
    ~BaseCallScope() { tBaseCall = mSaved; }

private:
    BaseCallMarker mSaved;
};

struct PerIsolateData;
struct PerContextData;

struct WrapperKey {
    const void* mAddress = nullptr;
    const TypeNode* mRoot = nullptr;

    friend bool operator==(const WrapperKey&, const WrapperKey&) = default;
};

struct WrapperKeyHash {
    [[nodiscard]] auto operator()(const WrapperKey& key) const noexcept -> std::size_t {
        const auto a = std::hash<const void*>{}(key.mAddress);
        const auto b = std::hash<const void*>{}(key.mRoot);
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    }
};

struct TransparentStringHash {
    using is_transparent = void;
    [[nodiscard]] auto operator()(const std::string_view text) const noexcept -> std::size_t {
        return std::hash<std::string_view>{}(text);
    }
};

struct RetainedCell {
    std::shared_ptr<ValueState> mValue; // weak once slotted
    std::shared_ptr<ValueState> mOwner; // weak: the owner's wrapper
    std::uint32_t mSlot = 0;
};

struct PathKey {
    const void* mSource = nullptr;
    std::size_t mIndex = 0;

    friend bool operator==(const PathKey&, const PathKey&) = default;
};

struct PathKeyHash {
    [[nodiscard]] auto operator()(const PathKey& key) const noexcept -> std::size_t {
        const auto a = std::hash<const void*>{}(key.mSource);
        const auto b = std::hash<std::size_t>{}(key.mIndex);
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    }
};

struct WrapperEntry {
    v8::Global<v8::Object> mHandle; // weak
    std::unique_ptr<Holder> mHolder;
    WrapperEntry* mPrev = nullptr; // in its isolate's live or finalizing list
    WrapperEntry* mNext = nullptr;
    PerIsolateData* mData = nullptr;  // null once orphaned by disposeIsolate
    PerContextData* mCache = nullptr; // identity-cache membership (may be null)
    WrapperKey mKey;
    const TypeNode* mNode = nullptr; // null for non-class entries
    std::shared_ptr<WrapperEntry*> mSelf;
    std::vector<std::weak_ptr<RetainedCell>> mStrongRetained;
    std::unique_ptr<std::unordered_map<PathKey, v8::Global<v8::Object>, PathKeyHash>> mPaths;
    std::size_t mPathSweepAt = 64; // when to drop the collected ones

    WrapperEntry() = default;
    WrapperEntry(const WrapperEntry&) = delete;
    WrapperEntry& operator=(const WrapperEntry&) = delete;
    ~WrapperEntry() {
        if (mSelf) {
            *mSelf = nullptr;
        }
    }
};

using SyntheticModuleFactory = v8::Local<v8::Module> (*)(v8::Isolate* isolate);

struct ModuleSlot {
    SyntheticModuleFactory mFactory = nullptr;
    v8::Global<v8::Module> mModule;
};

class EntryList {
public:
    void push(WrapperEntry* entry) noexcept {
        entry->mPrev = nullptr;
        entry->mNext = mHead;
        if (mHead != nullptr) {
            mHead->mPrev = entry;
        }
        mHead = entry;
    }

    void remove(WrapperEntry* entry) noexcept {
        (entry->mPrev != nullptr ? entry->mPrev->mNext : mHead) = entry->mNext;
        if (entry->mNext != nullptr) {
            entry->mNext->mPrev = entry->mPrev;
        }
        entry->mPrev = entry->mNext = nullptr;
    }

    template <class F> void forEach(F&& fn) {
        for (auto* entry = mHead; entry != nullptr;) {
            auto* const next = entry->mNext;
            fn(entry);
            entry = next;
        }
    }

    void clear() noexcept { mHead = nullptr; }
    [[nodiscard]] bool empty() const noexcept { return mHead == nullptr; }

private:
    WrapperEntry* mHead = nullptr;
};

class WrapperCache {
public:
    [[nodiscard]] auto find(const WrapperKey& key) const noexcept -> WrapperEntry* {
        if (mSlots.empty()) {
            return nullptr;
        }
        for (auto i = indexOf(key);; i = (i + 1) & (mSlots.size() - 1)) {
            const auto& [mKey, mEntry] = mSlots[i];
            if (mEntry == nullptr) {
                return nullptr;
            }
            if (mEntry != tombstone() && mKey == key) {
                return mEntry;
            }
        }
    }

    void assign(const WrapperKey& key, WrapperEntry* entry) {
        if ((mUsed + 1) * 2 > mSlots.size()) {
            rehash();
        }
        Slot* reuse = nullptr;
        for (auto i = indexOf(key);; i = (i + 1) & (mSlots.size() - 1)) {
            auto& slot = mSlots[i];
            if (slot.mEntry == nullptr) {
                if (reuse == nullptr) {
                    reuse = &slot;
                    ++mUsed;
                }
                *reuse = Slot{key, entry};
                return;
            }
            if (slot.mEntry == tombstone()) {
                reuse = reuse != nullptr ? reuse : &slot;
            } else if (slot.mKey == key) {
                slot.mEntry = entry;
                return;
            }
        }
    }

    void erase(const WrapperKey& key, const WrapperEntry* only = nullptr) noexcept {
        if (mSlots.empty()) {
            return;
        }
        for (auto i = indexOf(key);; i = (i + 1) & (mSlots.size() - 1)) {
            auto& slot = mSlots[i];
            if (slot.mEntry == nullptr) {
                return;
            }
            if (slot.mEntry != tombstone() && slot.mKey == key) {
                if (only == nullptr || slot.mEntry == only) {
                    slot.mEntry = tombstone();
                }
                return;
            }
        }
    }

private:
    struct Slot {
        WrapperKey mKey;
        WrapperEntry* mEntry = nullptr;
    };

    [[nodiscard]] static auto tombstone() noexcept -> WrapperEntry* {
        return reinterpret_cast<WrapperEntry*>(std::uintptr_t{1});
    }
    [[nodiscard]] auto indexOf(const WrapperKey& key) const noexcept -> std::size_t {
        return (WrapperKeyHash{}(key) * 0x9E3779B97F4A7C15ULL >> 20) & (mSlots.size() - 1);
    }

    void rehash() {
        std::size_t live = 0;
        for (const auto& [mKey, mEntry] : mSlots) {
            live += mEntry != nullptr && mEntry != tombstone() ? 1 : 0;
        }
        auto capacity = (std::max)(std::size_t{16}, mSlots.size());
        while ((live + 1) * 4 > capacity) {
            capacity *= 2;
        }
        const auto old = std::exchange(mSlots, std::vector<Slot>(capacity));
        mUsed = 0;
        for (const auto& slot : old) {
            if (slot.mEntry != nullptr && slot.mEntry != tombstone()) {
                auto i = indexOf(slot.mKey);
                while (mSlots[i].mEntry != nullptr) {
                    i = (i + 1) & (mSlots.size() - 1);
                }
                mSlots[i] = slot;
                ++mUsed;
            }
        }
    }

    std::vector<Slot> mSlots; // a power of two, or empty
    std::size_t mUsed = 0;    // slots ever filled
};

struct PerContextData {
    const void* mInstance = nullptr;
    int mSlot = -1;                         // embedder-data slot
    v8::Global<v8::Context> mContext;       // weak: Alka never keeps a context alive
    PerIsolateData* mIsolateData = nullptr; // null once orphaned by disposeIsolate collected
    bool mDying = false;
    WrapperCache mWrapperCache;
    std::unordered_map<std::string, ModuleSlot, TransparentStringHash, std::equal_to<>> mModules;
};

struct PerIsolateData {
    v8::Isolate* mIsolate = nullptr;
    std::shared_ptr<IsolateLink> mLink;
    DestructionMode mDestructionMode = DestructionMode::Immediate;
    std::vector<std::unique_ptr<Holder>> mDeferred; // collected, not yet destroyed
    std::int64_t mExternalBytes = 0;                // currently reported to V8
#if ALKA_HAS_EXTERNAL_MEMORY_ACCOUNTER
    v8::ExternalMemoryAccounter mAccounter;
#endif
    std::unordered_map<const TypeNode*, v8::Global<v8::FunctionTemplate>> mClassTemplates;
    std::vector<v8::Global<v8::String>> mHotKeys;      // by hot-key slot
    std::vector<v8::Global<v8::Private>> mHotPrivates; // by hot-key slot
    std::unordered_map<std::string, v8::Global<v8::Symbol>, TransparentStringHash, std::equal_to<>>
        mWellKnownSymbols;
    EntryList mLiveWrappers;
    EntryList mFinalizing; // collected; second pass pending
    std::vector<std::unique_ptr<PerContextData>> mContexts;
    std::vector<std::function<void()>> mDisposeHooks; // see onDisposeIsolate
};

[[nodiscard]] auto perIsolateData(v8::Isolate* isolate) -> PerIsolateData&;
[[nodiscard]] auto perContextData(v8::Local<v8::Context> context) -> PerContextData&;
[[nodiscard]] auto isolateLink(v8::Isolate* isolate) -> const std::shared_ptr<IsolateLink>&;

inline thread_local int tNativeDepth = 0;
inline thread_local std::vector<std::shared_ptr<void>>* tPendingReleases = nullptr;

void releasePendingObjects();

struct NativeCallScope {
    NativeCallScope() noexcept { ++tNativeDepth; }
    NativeCallScope(const NativeCallScope&) = delete;
    NativeCallScope& operator=(const NativeCallScope&) = delete;
    ~NativeCallScope() {
        if (--tNativeDepth == 0 && tPendingReleases != nullptr && !tPendingReleases->empty()) {
            releasePendingObjects();
        }
    }
};

void releaseWhenUnused(std::shared_ptr<void> owner);

void disposeFromJs(v8::Isolate* isolate, v8::Local<v8::Object> self);

inline void enterCallback(v8::Isolate* isolate) {
    const auto& link = isolateLink(isolate);
    link->noteLocker();
    if (link->hasPending()) {
        link->runPending();
    }
}

struct KnownType {
    const TypeNode* mNode = nullptr;
    void* (*mFromComplete)(void* complete) = nullptr;
};

void registerTypeNodeIndex(const std::type_info& id, const TypeNode* node,
                           void* (*fromComplete)(void*));
[[nodiscard]] auto lookupTypeNodeByTypeId(const std::type_info& id) -> KnownType;

[[nodiscard]] bool isKnownTypeNode(const TypeNode* node);

[[nodiscard]] auto instantiateWrapper(ConvertCtx& cx, const TypeNode*& node, void*& ptr)
    -> v8::Local<v8::Object>;

[[nodiscard]] auto wrapNative(ConvertCtx& cx, const TypeNode* node, std::unique_ptr<Holder> holder,
                              const void* cacheKey) -> v8::Local<v8::Object>;

[[nodiscard]] auto findCachedWrapper(ConvertCtx& cx, const TypeNode* node, const void* key,
                                     std::unique_ptr<Holder>* adopt = nullptr)
    -> v8::Local<v8::Object>;

void attachWrapper(v8::Local<v8::Context> context, v8::Local<v8::Object> target,
                   const TypeNode* node, std::unique_ptr<Holder> holder, const void* cacheKey);

void trackExternalHolder(v8::Isolate* isolate, v8::Local<v8::Object> object,
                         std::unique_ptr<Holder> holder);

void invalidateWrappers(v8::Isolate* isolate, const TypeNode* node, const void* key);

void retainOwner(ConvertCtx& cx, v8::Local<v8::Value> child, v8::Local<v8::Object> owner);

[[nodiscard]] auto findPath(v8::Isolate* isolate, const WrapperEntry& owner, const PathKey& key)
    -> v8::Local<v8::Object>;

void rememberPath(v8::Isolate* isolate, WrapperEntry& owner, const PathKey& key,
                  v8::Local<v8::Object> view);

void keepAliveValue(ConvertCtx& cx, v8::Local<v8::Object> self, v8::Local<v8::Value> value);

void releaseKeptValues(ConvertCtx& cx, v8::Local<v8::Object> self);
void releaseKeptValues(v8::Isolate* isolate, const TypeNode* node, const void* key);

[[nodiscard]] auto pinnedOwner(Holder& holder) -> std::shared_ptr<void>;

void adjustExternalMemory(PerIsolateData& data, std::int64_t delta);

[[nodiscard]] auto findOwningHolder(v8::Isolate* isolate, const TypeNode* node, const void* key)
    -> Holder*;

[[nodiscard]] auto retainedStore(ConvertCtx& cx, v8::Local<v8::Object> owner, bool create)
    -> v8::Local<v8::Map>;
[[nodiscard]] auto nextRetainedSlot() -> std::uint32_t;

[[nodiscard]] bool ownerKeepsValues(v8::Local<v8::Object> owner);

[[nodiscard]] auto keepingWrapper(v8::Isolate* isolate, v8::Local<v8::Object> owner)
    -> v8::Local<v8::Object>;

void slotRetained(ConvertCtx& cx, v8::Local<v8::Object> owner, RetainedCell& cell,
                  v8::Local<v8::Value> value);

void trackStrongRetained(v8::Local<v8::Object> owner, const std::shared_ptr<RetainedCell>& cell);

void migrateRetained(ConvertCtx& cx, WrapperEntry& entry, v8::Local<v8::Object> wrapper);

[[nodiscard]] auto registeredErrorValue(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                        const std::exception_ptr& error) -> v8::Local<v8::Value>;

void throwCppException(v8::Isolate* isolate, const std::exception_ptr& error,
                       std::string_view where);

void throwJsException(v8::Isolate* isolate, std::string_view message,
                      const std::shared_ptr<ValueState>& original);

[[nodiscard]] auto caughtError(v8::Isolate* isolate, v8::Local<v8::Context> context,
                               v8::TryCatch& tryCatch) -> ConvertError;

inline void throwConversionError(v8::Isolate* isolate, const ConvertError& error,
                                 const std::string_view message) {
    if (error.mJsException) {
        throwJsException(isolate, message, error.mJsException);
    } else {
        throwTypeError(isolate, message);
    }
}

[[nodiscard]] auto ensureNamespace(v8::Local<v8::Context> context, std::string_view dottedPath)
    -> v8::Local<v8::Object>;

void defineNamespaceValue(v8::Local<v8::Context> context, v8::Local<v8::Object> target,
                          std::string_view name, v8::Local<v8::Value> value, bool isReadonly);

void registerModuleFactory(v8::Local<v8::Context> context, std::string_view path,
                           SyntheticModuleFactory factory);

} // namespace Alka::Detail

namespace Alka {

void disposeIsolate(v8::Isolate* isolate);
void onDisposeIsolate(v8::Isolate* isolate, std::function<void()> hook);

} // namespace Alka
