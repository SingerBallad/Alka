#include <Alka/Install.hpp>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>

namespace Alka {

namespace Detail {

namespace {

std::shared_mutex gIsolatesMutex;
std::unordered_map<v8::Isolate*, std::unique_ptr<PerIsolateData>>& isolateRegistry() {
    static std::unordered_map<v8::Isolate*, std::unique_ptr<PerIsolateData>> registry;
    return registry;
}

void secondPassCallback(const v8::WeakCallbackInfo<WrapperEntry>& info) {
    const auto entry = info.GetParameter();
    delete entry->mHolder;  // the only place a GC'd native object is destroyed
    delete entry;
}

void firstPassCallback(const v8::WeakCallbackInfo<WrapperEntry>& info) {
    const auto entry = info.GetParameter();
    entry->mHandle.Reset();
    entry->mData->mLiveWrappers.erase(entry);
    if (entry->mCache != nullptr) {
        if (const auto it = entry->mCache->mWrapperCache.find(entry->mKey);
            it != entry->mCache->mWrapperCache.end() && it->second == entry) {
            entry->mCache->mWrapperCache.erase(it);
        }
    }
    info.SetSecondPassCallback(&secondPassCallback);
}

}  // namespace

auto perIsolateData(v8::Isolate* isolate) -> PerIsolateData& {
    {
        std::shared_lock lock(gIsolatesMutex);
        if (const auto it = isolateRegistry().find(isolate); it != isolateRegistry().end()) {
            return *it->second;
        }
    }
    std::unique_lock lock(gIsolatesMutex);
    auto& slot = isolateRegistry()[isolate];
    if (!slot) {
        slot = std::make_unique<PerIsolateData>();
        slot->mIsolate = isolate;
    }
    return *slot;
}

namespace {

std::shared_mutex gTypeIndexMutex;
auto typeIndexRegistry() -> std::unordered_map<std::type_index, const TypeNode*>& {
    static std::unordered_map<std::type_index, const TypeNode*> registry;
    return registry;
}

}  // namespace

void registerTypeNodeIndex(const std::type_info& id, const TypeNode* node) {
    std::unique_lock lock(gTypeIndexMutex);
    typeIndexRegistry().emplace(std::type_index(id), node);
}

const TypeNode* lookupTypeNodeByTypeId(const std::type_info& id) {
    std::shared_lock lock(gTypeIndexMutex);
    const auto& registry = typeIndexRegistry();
    const auto it = registry.find(std::type_index(id));
    return it != registry.end() ? it->second : nullptr;
}

auto perContextData(const v8::Local<v8::Context> context) -> PerContextData& {
    // V8 14.x removed Context::GetIsolate() and requires an isolate for the
    // embedder-data accessors; the isolate is always entered on this thread here.
    const auto isolate = v8::Isolate::GetCurrent();
    if (static_cast<std::uint32_t>(ALKA_CONTEXT_EMBEDDER_SLOT) <
        context->GetNumberOfEmbedderDataFields()) {
        if (const auto existing =
                context->GetAlignedPointerFromEmbedderData(isolate, ALKA_CONTEXT_EMBEDDER_SLOT);
            existing != nullptr) {
            return *static_cast<PerContextData*>(existing);
        }
    }
    auto& data = perIsolateData(isolate);
    auto owned = std::make_unique<PerContextData>();
    const auto pcd = owned.get();
    pcd->mIsolateData = &data;
    pcd->mContext.Reset(isolate, context);
    data.mContexts.push_back(std::move(owned));
    context->SetAlignedPointerInEmbedderData(ALKA_CONTEXT_EMBEDDER_SLOT, pcd);
    return *pcd;
}

namespace {

bool nodeOnChain(const TypeNode* from, const TypeNode* target) {
    for (auto n = from; n != nullptr; n = n->mParent) {
        if (n == target) {
            return true;
        }
    }
    return false;
}

}  // namespace

auto wrapNative(const ConvertCtx& cx, const TypeNode* node, Holder* holder, void* cacheKey)
    -> v8::Local<v8::Object> {
    auto& data = perIsolateData(cx.mIsolate);

    auto current = node;
    while (current != nullptr) {
        if (const auto it = data.mClassTemplates.find(current); it != data.mClassTemplates.end()) {
            v8::EscapableHandleScope scope(cx.mIsolate);
            const auto ft = it->second.Get(cx.mIsolate);
            v8::Local<v8::Object> object;
            if (!ft->InstanceTemplate()->NewInstance(cx.mContext).ToLocal(&object)) {
                delete holder;
                throw JsException("failed to instantiate wrapper for " +
                                  std::string(current->mName));
            }
            attachWrapper(cx.mContext, object, current, holder, cacheKey);
            return scope.Escape(object);
        }
        if (current->mUpcast != nullptr) {
            holder->mPtr = current->mUpcast(holder->mPtr);
        }
        current = current->mParent;
    }
    const std::string name(node->mName);
    delete holder;
    throw JsException("class " + name + " (or any of its bases) is not installed in this isolate");
}

auto findCachedWrapper(ConvertCtx& cx, const TypeNode* node, void* key)
    -> v8::Local<v8::Object> {
    auto& pcd = perContextData(cx.mContext);
    const auto it = pcd.mWrapperCache.find(key);
    if (it == pcd.mWrapperCache.end()) {
        return {};
    }
    const auto entry = it->second;
    if (!nodeOnChain(entry->mNode, node) && !nodeOnChain(node, entry->mNode)) {
        return {};
    }
    return entry->mHandle.Get(cx.mIsolate);
}

void attachWrapper(const v8::Local<v8::Context> context, const v8::Local<v8::Object> target,
                   const TypeNode* node, Holder* holder, void* cacheKey) {
    const auto isolate = v8::Isolate::GetCurrent();
    target->SetAlignedPointerInInternalField(kNodeField,
                                             const_cast<void*>(static_cast<const void*>(node)));
    target->SetAlignedPointerInInternalField(kHolderField, holder);

    auto& data = perIsolateData(isolate);
    auto& pcd = perContextData(context);
    auto* entry = new WrapperEntry;
    entry->mHolder = holder;
    entry->mData = &data;
    entry->mNode = node;
    entry->mKey = cacheKey;
    entry->mCache = &pcd;
    entry->mHandle.Reset(isolate, target);
    entry->mHandle.SetWeak(entry, &firstPassCallback, v8::WeakCallbackType::kParameter);
    data.mLiveWrappers.insert(entry);
    pcd.mWrapperCache[cacheKey] = entry;  // newest wrapper wins the identity slot
}

void trackExternalHolder(v8::Isolate* isolate, v8::Local<v8::Object> object, Holder* holder) {
    auto& data = perIsolateData(isolate);
    auto* entry = new WrapperEntry;
    entry->mHolder = holder;
    entry->mData = &data;
    entry->mHandle.Reset(isolate, object);
    entry->mHandle.SetWeak(entry, &firstPassCallback, v8::WeakCallbackType::kParameter);
    data.mLiveWrappers.insert(entry);
}

namespace {

auto internedString(v8::Isolate* isolate, const std::string_view text) -> v8::Local<v8::String> {
    return v8Str(isolate, text, true);
}

}  // namespace

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

        const auto key = internedString(isolate, part);
        v8::Local<v8::Value> existing;
        if (current->GetRealNamedProperty(context, key).ToLocal(&existing) &&
            existing->IsObject()) {
            current = existing.As<v8::Object>();
        } else {
            const auto fresh = v8::Object::New(isolate);
            current
                ->DefineOwnProperty(
                    context, key, fresh,
                    static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete))
                .Check();
            current = fresh;
        }
    }
    return scope.Escape(current);
}

auto defineNamespaceValue(const v8::Local<v8::Context> context, const v8::Local<v8::Object> target,
                          const std::string_view name, const v8::Local<v8::Value> value, bool isReadonly)
    -> void {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    const auto attributes = isReadonly
                                ? static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete)
                                : v8::None;
    target->DefineOwnProperty(context, internedString(isolate, name), value, attributes).Check();
}

}  // namespace Detail

void disposeIsolate(v8::Isolate* isolate) {
    std::unique_ptr<Detail::PerIsolateData> data;
    {
        std::unique_lock lock(Detail::gIsolatesMutex);
        auto& registry = Detail::isolateRegistry();
        if (const auto it = registry.find(isolate); it != registry.end()) {
            data = std::move(it->second);
            registry.erase(it);
        }
    }
    if (!data) {
        return;
    }
    for (const auto entry : data->mLiveWrappers) {
        entry->mHandle.Reset();
        delete entry->mHolder;
        delete entry;
    }
    data->mLiveWrappers.clear();
    data->mClassTemplates.clear();
}

}  // namespace Alka
