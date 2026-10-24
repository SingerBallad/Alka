#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Runtime/Install.hpp>

#include <v8.h>

#include <string_view>
#include <vector>

namespace Alka {

namespace Detail {

template <BoundModule Mod> [[nodiscard]] auto moduleExportNames() -> std::vector<std::string_view> {
    std::vector<std::string_view> names;
    forEachMember<Mod>([&]<typename D>(const D&) {
        if constexpr (D::kind == MemberKind::ClassRef) {
            static_assert(validateExport<typename D::Type>());
            if constexpr (BoundClass<typename D::Type>) {
                names.push_back(kTypeNode<typename D::Type>.mName);
            }
        } else if constexpr (D::kind == MemberKind::Function || D::kind == MemberKind::Var ||
                             D::kind == MemberKind::EnumType || D::kind == MemberKind::ErrorType) {
            names.push_back(D::name);
        }
    });
    return names;
}

template <BoundModule Mod>
auto evaluateSyntheticModule(v8::Local<v8::Context> context, v8::Local<v8::Module> module)
    -> v8::MaybeLocal<v8::Value> {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::EscapableHandleScope scope(isolate);
    const auto exports = v8::Object::New(isolate);
    installMembers<Mod>(context, exports, false);
    for (const std::string_view name : moduleExportNames<Mod>()) {
        const auto key = v8Str(isolate, name, true);
        v8::Local<v8::Value> value;
        if (!exports->Get(context, key).ToLocal(&value) ||
            module->SetSyntheticModuleExport(isolate, key, value).IsNothing()) {
            return {};
        }
    }
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
        resolver->Resolve(context, v8::Undefined(isolate)).IsNothing()) {
        return {};
    }
    return scope.Escape(resolver->GetPromise().As<v8::Value>());
}

template <BoundModule Mod>
auto createSyntheticModule(v8::Isolate* isolate) -> v8::Local<v8::Module> {
    std::vector<v8::Local<v8::String>> names;
    for (const std::string_view name : moduleExportNames<Mod>()) {
        names.push_back(v8Str(isolate, name, true));
    }
    return v8::Module::CreateSyntheticModule(
        isolate, v8Str(isolate, modulePathOf<Mod>()),
        v8::MemorySpan<const v8::Local<v8::String>>(names.data(), names.size()),
        &evaluateSyntheticModule<Mod>);
}

template <BoundModule Mod> void registerModuleTree(v8::Local<v8::Context> context) {
    registerModuleFactory(context, modulePathOf<Mod>(), &createSyntheticModule<Mod>);
    forEachMember<Mod>([&]<typename D>(const D&) {
        if constexpr (D::kind == MemberKind::Submodule) {
            registerModuleTree<typename D::Module>(context);
        }
    });
}

} // namespace Detail

template <BoundModule... Mods> void registerModules(v8::Local<v8::Context> context) {
    static_assert(Detail::checkModulePack<true, Mods...>());
    (Detail::registerModuleTree<Mods>(context), ...);
}

[[nodiscard]] auto resolveModule(v8::Local<v8::Context> context, v8::Local<v8::String> specifier)
    -> v8::MaybeLocal<v8::Module>;

auto moduleResolveCallback(v8::Local<v8::Context> context, v8::Local<v8::String> specifier,
                           v8::Local<v8::FixedArray> importAttributes,
                           v8::Local<v8::Module> referrer) -> v8::MaybeLocal<v8::Module>;

} // namespace Alka
