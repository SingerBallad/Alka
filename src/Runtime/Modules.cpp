#include <Alka/Detail/State.hpp>
#include <Alka/Runtime/Modules.hpp>

#include <string>
#include <string_view>

namespace Alka {

namespace Detail {

void registerModuleFactory(const v8::Local<v8::Context> context, const std::string_view path,
                           const SyntheticModuleFactory factory) {
    auto& [mFactory, mModule] = perContextData(context).mModules[std::string(path)];
    if (mFactory != nullptr && mFactory != factory) {
        throw JsException("Alka: two modules registered under the path '" + std::string(path) +
                          "'");
    }
    mFactory = factory;
}

} // namespace Detail

auto resolveModule(const v8::Local<v8::Context> context, const v8::Local<v8::String> specifier)
    -> v8::MaybeLocal<v8::Module> {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::EscapableHandleScope scope(isolate);
    const v8::String::Utf8Value name(isolate, specifier);
    if (*name == nullptr) {
        return {};
    }
    auto& modules = Detail::perContextData(context).mModules;
    const auto it = modules.find(std::string_view(*name, static_cast<std::size_t>(name.length())));
    if (it == modules.end()) {
        return {};
    }
    auto& slot = it->second;
    if (slot.mModule.IsEmpty()) {
        slot.mModule.Reset(isolate, slot.mFactory(isolate));
        slot.mModule.SetWeak();
    }
    return scope.Escape(slot.mModule.Get(isolate));
}

auto moduleResolveCallback(const v8::Local<v8::Context> context,
                           const v8::Local<v8::String> specifier, v8::Local<v8::FixedArray>,
                           v8::Local<v8::Module>) -> v8::MaybeLocal<v8::Module> {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::EscapableHandleScope scope(isolate);
    v8::Local<v8::Module> module;
    if (resolveModule(context, specifier).ToLocal(&module)) {
        return scope.Escape(module);
    }
    const v8::String::Utf8Value name(isolate, specifier);
    Detail::throwError(isolate,
                       "Cannot find module '" + std::string(*name != nullptr ? *name : "") + "'");
    return {};
}

} // namespace Alka
