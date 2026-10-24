#include <Alka/Detail/Compat.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Runtime/Errors.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <typeindex>
#include <vector>

namespace Alka::Detail {

namespace {

auto errorsMutex() -> std::shared_mutex& {
    static auto& mutex = *new std::shared_mutex;
    return mutex;
}
std::atomic gAnyErrors{false};

struct Registered {
    ErrorTypeInfo mInfo;
    std::size_t mDepth = 0;
    std::size_t mOrder = 0;
};

auto registry() -> std::vector<Registered>& {
    static auto& entries = *new std::vector<Registered>;
    return entries;
}

void reorder(std::vector<Registered>& entries) {
    const auto depthOf = [&](const Registered& entry) {
        std::size_t depth = 0;
        for (const std::type_info* base = entry.mInfo.mBase; base != nullptr && depth < 64;
             ++depth) {
            const auto it = std::ranges::find_if(
                entries, [&](const Registered& e) { return *e.mInfo.mType == *base; });
            base = it != entries.end() ? it->mInfo.mBase : nullptr;
        }
        return depth;
    };
    for (auto& entry : entries) {
        entry.mDepth = depthOf(entry);
    }
    std::ranges::sort(entries, [](const Registered& a, const Registered& b) {
        return a.mDepth != b.mDepth ? a.mDepth > b.mDepth : a.mOrder > b.mOrder;
    });
}

auto intrinsicErrorClass(v8::Isolate* isolate, const v8::Local<v8::Context> context)
    -> v8::MaybeLocal<v8::Function> {
    const auto sample = v8::Exception::Error(v8Str(isolate, "")).As<v8::Object>();
    const v8::Local<v8::Value> prototype = prototypeOf(sample);
    v8::Local<v8::Value> constructor;
    if (!prototype->IsObject() ||
        !prototype.As<v8::Object>()
             ->Get(context, v8Str(isolate, "constructor", true))
             .ToLocal(&constructor) ||
        !constructor->IsFunction()) {
        return {};
    }
    return constructor.As<v8::Function>();
}

} // namespace

void registerErrorType(ErrorTypeInfo info) {
    const std::unique_lock lock(errorsMutex());
    auto& entries = registry();
    if (const auto it = std::ranges::find_if(
            entries, [&](const Registered& e) { return *e.mInfo.mType == *info.mType; });
        it != entries.end()) {
        if (!it->mInfo.mDecorate) {
            it->mInfo.mDecorate = std::move(info.mDecorate);
        }
        return;
    }
    entries.push_back(Registered{std::move(info), 0, entries.size()});
    reorder(entries);
    gAnyErrors.store(true, std::memory_order_release);
}

auto errorClass(const v8::Local<v8::Context> context, const std::type_info& type)
    -> v8::MaybeLocal<v8::Function> {
    std::string name;
    const std::type_info* base = nullptr;
    {
        const std::shared_lock lock(errorsMutex());
        const auto& entries = registry();
        const auto it = std::ranges::find_if(
            entries, [&](const Registered& e) { return *e.mInfo.mType == type; });
        if (it == entries.end()) {
            return {};
        }
        name = it->mInfo.mName;
        base = it->mInfo.mBase;
    }
    const auto isolate = v8::Isolate::GetCurrent();
    v8::EscapableHandleScope scope(isolate);
    const v8::Context::Scope contextScope(context);

    const auto global = context->Global();
    const auto cacheKey = cachedPrivate<"Alka::errorClasses">(isolate);
    v8::Local<v8::Value> existing;
    v8::Local<v8::Map> cache;
    if (global->GetPrivate(context, cacheKey).ToLocal(&existing) && existing->IsMap()) {
        cache = existing.As<v8::Map>();
    } else {
        cache = v8::Map::New(isolate);
        (void)global->SetPrivate(context, cacheKey, cache);
    }
    const auto nameKey = v8Str(isolate, name, true);
    v8::Local<v8::Value> cached;
    if (cache->Get(context, nameKey).ToLocal(&cached) && cached->IsFunction()) {
        return scope.Escape(cached.As<v8::Function>());
    }

    v8::Local<v8::Function> baseClass;
    if (base != nullptr ? !errorClass(context, *base).ToLocal(&baseClass)
                        : !intrinsicErrorClass(isolate, context).ToLocal(&baseClass)) {
        return {};
    }
    static constexpr std::string_view kFactory =
        "(function (Base, name) {"
        "  const C = class extends Base {};"
        "  Object.defineProperty(C, 'name', { value: name });"
        "  Object.defineProperty(C.prototype, 'name',"
        "                        { value: name, writable: true, configurable: true });"
        "  return C;"
        "})";
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> factory;
    v8::Local<v8::Value> made;
    v8::Local<v8::Value> argv[] = {baseClass, nameKey};
    if (!v8::Script::Compile(context, v8Str(isolate, kFactory)).ToLocal(&script) ||
        !script->Run(context).ToLocal(&factory) || !factory->IsFunction() ||
        !factory.As<v8::Function>()
             ->Call(context, v8::Undefined(isolate), 2, argv)
             .ToLocal(&made) ||
        !made->IsFunction()) {
        return {};
    }
    std::ignore = cache->Set(context, nameKey, made);
    return scope.Escape(made.As<v8::Function>());
}

auto registeredErrorValue(v8::Isolate* isolate, const v8::Local<v8::Context> context,
                          const std::exception_ptr& error) -> v8::Local<v8::Value> {
    if (!gAnyErrors.load(std::memory_order_acquire) || !error) {
        return {};
    }
    std::vector<Registered> entries;
    {
        const std::shared_lock lock(errorsMutex());
        entries = registry();
    }
    v8::EscapableHandleScope scope(isolate);
    const v8::Context::Scope contextScope(context);
    ConvertCtx cx{isolate, context};
    for (const auto& entry : entries) {
        v8::Local<v8::Function> cls;
        if (!errorClass(context, *entry.mInfo.mType).ToLocal(&cls)) {
            continue;
        }
        v8::Local<v8::Value> out;
        if (entry.mInfo.mMatch(error, cx, cls, entry.mInfo.mDecorate.get(), out)) {
            return out.IsEmpty() ? v8::Local<v8::Value>() : scope.Escape(out);
        }
    }
    return {};
}

void throwCppException(v8::Isolate* isolate, const std::exception_ptr& error,
                       const std::string_view where) {
    if (isolate->IsExecutionTerminating()) {
        return;
    }
    if (const auto value = registeredErrorValue(isolate, isolate->GetCurrentContext(), error);
        !value.IsEmpty()) {
        isolate->ThrowException(value);
        return;
    }
    std::string message;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        message = e.what();
    } catch (...) {
        message = "unknown C++ exception in " + std::string(where);
    }
    throwError(isolate, message);
}

} // namespace Alka::Detail
