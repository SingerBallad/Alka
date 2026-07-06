#pragma once

#include <Alka/Bind.hpp>
#include <Alka/Converter.hpp>
#include <Alka/Holder.hpp>
#include <Alka/Invoke.hpp>
#include <Alka/TypeNode.hpp>

#include <v8.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Alka {

struct InstallOptions {};

void disposeIsolate(v8::Isolate* isolate);

namespace Detail {

struct PerIsolateData;
struct PerContextData;

struct WrapperEntry {
    v8::Global<v8::Object> mHandle;
    Holder* mHolder = nullptr;
    PerIsolateData* mData = nullptr;
    PerContextData* mCache = nullptr;  // identity-cache membership (may be null)
    void* mKey = nullptr;
    const TypeNode* mNode = nullptr;
};

struct PerContextData {
    v8::Global<v8::Context> mContext;
    PerIsolateData* mIsolateData = nullptr;
    std::unordered_map<void*, WrapperEntry*> mWrapperCache;
};

struct PerIsolateData {
    v8::Isolate* mIsolate = nullptr;
    std::unordered_map<const TypeNode*, v8::Global<v8::FunctionTemplate>> mClassTemplates;
    std::unordered_set<WrapperEntry*> mLiveWrappers;
    std::vector<std::unique_ptr<PerContextData>> mContexts;
};

[[nodiscard]] auto perIsolateData(v8::Isolate* isolate) -> PerIsolateData&;
[[nodiscard]] auto perContextData(v8::Local<v8::Context> context) -> PerContextData&;

[[nodiscard]] auto findCachedWrapper(ConvertCtx& cx, const TypeNode* node, void* key)
    -> v8::Local<v8::Object>;


void attachWrapper(v8::Local<v8::Context> context, v8::Local<v8::Object> target,
                   const TypeNode* node, Holder* holder, void* cacheKey);

void trackExternalHolder(v8::Isolate* isolate, v8::Local<v8::Object> object, Holder* holder);

[[nodiscard]] auto ensureNamespace(v8::Local<v8::Context> context, std::string_view dottedPath)
    -> v8::Local<v8::Object>;

void defineNamespaceValue(v8::Local<v8::Context> context, v8::Local<v8::Object> target,
                          std::string_view name, v8::Local<v8::Value> value, bool isReadonly);

template<class Tuple>
[[nodiscard]] consteval auto firstKindIndex(MemberKind kind) -> int {
    int found = -1;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (void)((std::tuple_element_t<Is, Tuple>::kind == kind && found < 0
                    ? (found = static_cast<int>(Is), true)
                    : false) ||
               ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    return found;
}

template<class Tuple>
[[nodiscard]] consteval auto kindCount(MemberKind kind) -> std::size_t {
    std::size_t n = 0;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((std::tuple_element_t<Is, Tuple>::kind == kind ? ++n : n), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    return n;
}

template<class Tuple>
[[nodiscard]] consteval auto kindIndicesOf(MemberKind kind) {
    std::array<std::size_t, std::tuple_size_v<Tuple> + 1> indices{};
    std::size_t count = 0;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((std::tuple_element_t<Is, Tuple>::kind == kind ? (indices[count++] = Is, 0) : 0), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    return std::pair{indices, count};
}

template<BoundClass T, class CtorD>
void constructAndAttach(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                        const CallSite& site, auto&& args) {
    guarded(cx.mIsolate, site, [&] {
        auto owned = std::apply(
            []<typename... T0>(T0&&... a) { return std::make_unique<T>(std::forward<T0>(a)...); },
            std::forward<decltype(args)>(args));
        T* ptr = owned.get();
        attachWrapper(cx.mContext, info.This(), &kTypeNode<T>,
                      new OwnedHolder<T>(std::move(owned)), ptr);
    });
}

template<BoundClass T, std::size_t Idx>
bool tryCtorOverload(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                     const CallSite& site) {
    using MembersTuple = std::remove_cvref_t<decltype(bindingMembers<T>())>;
    using CtorD = std::tuple_element_t<Idx, MembersTuple>;
    using ArgsTuple = CtorD::ArgsTuple;
    []<class... A>(std::type_identity<std::tuple<A...>>) {
        static_assert(std::is_constructible_v<T, A...>,
                      "Alka: ctor<Args...> does not match any constructor of T");
    }(std::type_identity<ArgsTuple>{});
    const auto& desc = std::get<Idx>(bindingMembers<T>());
    static_assert(!anySpecDefault<CtorD>() || kindCount<MembersTuple>(MemberKind::Ctor) == 1,
                  "Alka: arg() defaults on constructor overload sets are ambiguous");

    const auto argc = static_cast<std::size_t>(info.Length());
    if (argc < minArityOf<CtorD, CtorD::arity>() || argc > CtorD::arity) {
        return false;
    }
    auto args = tryConvertArguments<ArgsTuple>(info, cx, desc,
                                               std::make_index_sequence<CtorD::arity>{});
    if (!args) {
        return false;
    }
    constructAndAttach<T, CtorD>(info, cx, site, std::move(*args));
    return true;
}

template<BoundClass T>
void ctorCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    const auto* site = static_cast<const CallSite*>(info.Data().As<v8::External>()->Value());
    if (!info.IsConstructCall()) {
        throwTypeError(isolate, "Class constructor " + std::string(kTypeNode<T>.mName) +
                                    " cannot be invoked without 'new'");
        return;
    }

    using MembersTuple = std::remove_cvref_t<decltype(bindingMembers<T>())>;
    constexpr std::size_t ctorCount = kindCount<MembersTuple>(MemberKind::Ctor);
    if constexpr (ctorCount == 0) {
        throwTypeError(isolate, std::string(kTypeNode<T>.mName) +
                                    " cannot be constructed from JS (no ctor<> bound)");
    } else if constexpr (ctorCount == 1) {
        constexpr int ctorIndex = firstKindIndex<MembersTuple>(MemberKind::Ctor);
        using CtorD = std::tuple_element_t<static_cast<std::size_t>(ctorIndex), MembersTuple>;
        using ArgsTuple = CtorD::ArgsTuple;
        []<class... A>(std::type_identity<std::tuple<A...>>) {
            static_assert(std::is_constructible_v<T, A...>,
                          "Alka: ctor<Args...> does not match any constructor of T");
        }(std::type_identity<ArgsTuple>{});
        const auto& desc = std::get<static_cast<std::size_t>(ctorIndex)>(bindingMembers<T>());

        ConvertCtx cx{isolate, isolate->GetCurrentContext()};
        auto args = convertArguments<ArgsTuple>(info, cx, *site, desc,
                                                std::make_index_sequence<CtorD::arity>{});
        if (!args) {
            return;
        }
        constructAndAttach<T, CtorD>(info, cx, *site, std::move(*args));
    } else {
        ConvertCtx cx{isolate, isolate->GetCurrentContext()};
        constexpr auto ctors = kindIndicesOf<MembersTuple>(MemberKind::Ctor);
        const bool handled = [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            return (tryCtorOverload<T, ctors.first[Js]>(info, cx, *site) || ...);
        }(std::make_index_sequence<ctorCount>{});
        if (!handled) {
            throwTypeError(isolate, "no constructor overload of " +
                                        std::string(kTypeNode<T>.mName) +
                                        " matches the given arguments " + describeArgs(info));
        }
    }
}

template<class T, class Desc>
void installMethodMember(v8::Isolate* isolate, const v8::Local<v8::FunctionTemplate> classFt,
                         const v8::Local<v8::Signature> signature, const Desc& desc) {
    static const CallSite site = makeCallSite(kTypeNode<T>.mName, desc);
    v8::FunctionCallback callback;
    if constexpr (Desc::overloadCount == 1) {
        callback = &methodCallback<T, std::get<0>(Desc::callables), Desc>;
    } else {
        callback = &methodOverloadCallback<T, Desc>;
    }
    auto mft = v8::FunctionTemplate::New(
        isolate, callback, v8::External::New(isolate, const_cast<CallSite*>(&site)), signature);
    classFt->PrototypeTemplate()->Set(v8Str(isolate, Desc::name, true), mft, v8::DontEnum);
}

template<class T, class Desc>
void installStaticMember(v8::Isolate* isolate, const v8::Local<v8::FunctionTemplate> classFt,
                         const Desc& desc) {
    static const CallSite site = makeCallSite(kTypeNode<T>.mName, desc);
    v8::FunctionCallback callback;
    if constexpr (Desc::overloadCount == 1) {
        callback = &nativeCallback<std::get<0>(Desc::callables), Desc>;
    } else {
        callback = &freeOverloadCallback<Desc>;
    }
    auto mft = v8::FunctionTemplate::New(
        isolate, callback, v8::External::New(isolate, const_cast<CallSite*>(&site)));
    classFt->Set(v8Str(isolate, Desc::name, true), mft, v8::DontEnum);
}

template<class E, class Desc>
void installEnum(v8::Local<v8::Context> context, const v8::Local<v8::Object> ns, const Desc&) {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    const auto object = v8::Object::New(isolate);
    constexpr auto attributes =
        static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);
    for (const auto& entry : enumEntriesOf<E>) {
        const auto raw = static_cast<long long>(static_cast<std::underlying_type_t<E>>(entry.mValue));
        object
            ->DefineOwnProperty(context, v8Str(isolate, entry.mName, true),
                                v8::Number::New(isolate, static_cast<double>(raw)), attributes)
            .Check();
        object
            ->DefineOwnProperty(context, v8Str(isolate, std::to_string(raw), true),
                                v8Str(isolate, entry.mName), attributes)
            .Check();
    }
    object->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen).Check();
    defineNamespaceValue(context, ns, Desc::name, object, true);
}

template<class T, class Desc>
void installProperty(v8::Isolate* isolate, const v8::Local<v8::FunctionTemplate> classFt,
                     const v8::Local<v8::Signature> signature, const Desc& desc) {
    static const CallSite site = makeCallSite(kTypeNode<T>.mName, desc);
    const auto external = v8::External::New(isolate, const_cast<CallSite*>(&site));

    constexpr auto A0 = std::get<0>(Desc::accessors);
    v8::Local<v8::FunctionTemplate> getterFt;
    v8::Local<v8::FunctionTemplate> setterFt;

    if constexpr (Desc::isField) {
        getterFt = v8::FunctionTemplate::New(isolate, &fieldGetterCallback<T, A0>, external,
                                             signature);
        if (!desc.readonlyFlag()) {
            setterFt = v8::FunctionTemplate::New(isolate, &fieldSetterCallback<T, A0>, external,
                                                 signature);
        }
    } else {
        getterFt =
            v8::FunctionTemplate::New(isolate, &methodCallback<T, A0>, external, signature);
        if constexpr (std::tuple_size_v<decltype(Desc::accessors)> == 2) {
            if (!desc.readonlyFlag()) {
                constexpr auto A1 = std::get<1>(Desc::accessors);
                setterFt =
                    v8::FunctionTemplate::New(isolate, &methodCallback<T, A1>, external, signature);
            }
        }
    }
    classFt->PrototypeTemplate()->SetAccessorProperty(v8Str(isolate, Desc::name, true), getterFt,
                                                      setterFt, v8::DontEnum);
}

template<BoundClass T>
auto classTemplateOf(v8::Isolate* isolate) -> v8::Local<v8::FunctionTemplate> {
    static_assert(validateClassBinding<T>());
    auto& data = perIsolateData(isolate);
    if (const auto it = data.mClassTemplates.find(&kTypeNode<T>);
        it != data.mClassTemplates.end()) {
        return it->second.Get(isolate);
    }

    static const CallSite ctorSite = [] {
        CallSite s;
        s.mDisplayName = "new " + std::string(kTypeNode<T>.mName);
        forEachMember<T>([&]<typename T0>(const T0& d) {
            using D = std::remove_cvref_t<T0>;
            if constexpr (D::kind == MemberKind::Ctor) {
                if (s.mArgNames.empty()) {
                    d.forEachSpec([&]<typename T1>(const T1& spec) {
                        if constexpr (IsArgSpec<T1>) {
                            s.mArgNames.push_back(spec.mName);
                        }
                    });
                }
            }
        });
        return s;
    }();

    v8::Local<v8::FunctionTemplate> ft = v8::FunctionTemplate::New(
        isolate, &ctorCallback<T>, v8::External::New(isolate, const_cast<CallSite*>(&ctorSite)));
    ft->SetClassName(v8Str(isolate, kTypeNode<T>.mName, true));
    ft->InstanceTemplate()->SetInternalFieldCount(2);
    data.mClassTemplates.emplace(&kTypeNode<T>, v8::Global<v8::FunctionTemplate>(isolate, ft));
    registerTypeNodeIndex(typeid(T), &kTypeNode<T>);

    if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        ft->Inherit(classTemplateOf<ExtendsOf<T>>(isolate));
    }

    const auto signature = v8::Signature::New(isolate, ft);
    forEachMember<T>([&]<typename T0>(const T0& desc) {
        using Desc = std::remove_cvref_t<T0>;
        if constexpr (Desc::kind == MemberKind::Ctor) {
            // handled by ctorCallback
        } else if constexpr (Desc::kind == MemberKind::Method) {
            installMethodMember<T, Desc>(isolate, ft, signature, desc);
        } else if constexpr (Desc::kind == MemberKind::StaticMethod) {
            installStaticMember<T, Desc>(isolate, ft, desc);
        } else if constexpr (Desc::kind == MemberKind::Property) {
            installProperty<T, Desc>(isolate, ft, signature, desc);
        } else if constexpr (Desc::kind == MemberKind::Var) {
            // static constants materialize per context in classFunctionOf
        } else {
            static_assert(AlwaysFalse<Desc>,
                          "Alka: this member kind is not valid inside a class binding");
        }
    });
    return ft;
}

template<BoundClass T>
[[nodiscard]] auto classFunctionOf(v8::Local<v8::Context> context) -> v8::Local<v8::Function> {
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::Local<v8::Function> fn = classTemplateOf<T>(isolate)->GetFunction(context).ToLocalChecked();
    if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        fn->SetPrototypeV2(context, classFunctionOf<ExtendsOf<T>>(context)).Check();
    }
    ConvertCtx cx{isolate, context};
    forEachMember<T>([&]<typename T0>(const T0& desc) {
        using Desc = std::remove_cvref_t<T0>;
        if constexpr (Desc::kind == MemberKind::Var) {
            if (const v8::Local<v8::String> key = v8Str(isolate, Desc::name, true);
                !fn->HasOwnProperty(context, key).FromMaybe(true)) {
                (void)fn->DefineOwnProperty(
                    context, key, toJs(cx, desc.mValue),
                    static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete));
            }
        }
    });
    return fn;
}

template<BoundClass T>
void installClass(v8::Local<v8::Context> context, v8::Local<v8::Object> ns) {
    defineNamespaceValue(context, ns, kTypeNode<T>.mName, classFunctionOf<T>(context), true);
}

template<class Mod, class Desc>
void installFreeFunction(const v8::Local<v8::Context> context, v8::Local<v8::Object> ns,
                         const Desc& desc) {
    const auto isolate = v8::Isolate::GetCurrent();
    static const CallSite site = makeCallSite(modulePathOf<Mod>(), desc);
    v8::FunctionCallback callback;
    if constexpr (Desc::overloadCount == 1) {
        callback = &nativeCallback<std::get<0>(Desc::callables), Desc>;
    } else {
        callback = &freeOverloadCallback<Desc>;
    }
    const auto ft = v8::FunctionTemplate::New(
        isolate, callback, v8::External::New(isolate, const_cast<CallSite*>(&site)));
    defineNamespaceValue(context, ns, Desc::name, ft->GetFunction(context).ToLocalChecked(),
                         true);
}

template<BoundModule Mod>
void installModule(v8::Local<v8::Context> context) {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);
    v8::Local<v8::Object> ns = ensureNamespace(context, modulePathOf<Mod>());
    ConvertCtx cx{isolate, context};

    forEachMember<Mod>([&]<typename T0>(const T0& desc) {
        using Desc = std::remove_cvref_t<T0>;
        if constexpr (Desc::kind == MemberKind::Function) {
            installFreeFunction<Mod, Desc>(context, ns, desc);
        } else if constexpr (Desc::kind == MemberKind::Var) {
            defineNamespaceValue(context, ns, Desc::name, toJs(cx, desc.mValue), true);
        } else if constexpr (Desc::kind == MemberKind::Submodule) {
            installModule<typename Desc::Module>(context);
        } else if constexpr (Desc::kind == MemberKind::ClassRef) {
            installClass<typename Desc::Type>(context, ns);
        } else if constexpr (Desc::kind == MemberKind::EnumType) {
            installEnum<typename Desc::Type, Desc>(context, ns, desc);
        } else {
            static_assert(AlwaysFalse<Desc>,
                          "Alka: this member kind is not valid inside a module binding");
        }
    });
}

}  // namespace Detail

template<BoundModule... Mods>
void install(v8::Local<v8::Context> context, const InstallOptions& = {}) {
    (Detail::installModule<Mods>(context), ...);
}

// Registers a bound class's FunctionTemplate in the isolate WITHOUT installing
// it into any namespace. Use this when the host drives its own object graph and
// mints instances of T from C++ (Alka::toJs(cx, ptr) / Converter<T*|T|
// shared_ptr<T>>) instead of exposing T as a constructor via install<Module>.
// wrapNative() requires the template to be registered first; this is the
// public, namespace-free way to do that. Idempotent per isolate; also registers
// any bound base classes on T's inheritance chain.
template<BoundClass... Ts>
void registerClass(v8::Isolate* isolate) {
    (static_cast<void>(Detail::classTemplateOf<Ts>(isolate)), ...);
}

}  // namespace Alka
