#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Binding/Idioms.hpp>
#include <Alka/Convert/Converter.hpp>
#include <Alka/Detail/Compat.hpp>
#include <Alka/Detail/FastApi.hpp>
#include <Alka/Detail/Holder.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Detail/TypeNode.hpp>
#include <Alka/Runtime/Errors.hpp>

#include <v8.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Alka {

namespace Detail {

template <class Tuple> [[nodiscard]] consteval auto kindCount(MemberKind kind) -> std::size_t {
    std::size_t n = 0;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((std::tuple_element_t<Is, Tuple>::kind == kind ? ++n : n), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    return n;
}

template <class Tuple> [[nodiscard]] consteval auto kindIndicesOf(MemberKind kind) {
    std::array<std::size_t, std::tuple_size_v<Tuple> + 1> indices{};
    std::size_t count = 0;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((std::tuple_element_t<Is, Tuple>::kind == kind ? (indices[count++] = Is, 0) : 0), ...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    return std::pair{indices, count};
}

template <BoundClass T>
using ConstructedOf = decltype([] {
    if constexpr (HasTrampoline<T>) {
        return std::type_identity<typename Bind<T>::Trampoline>{};
    } else {
        return std::type_identity<T>{};
    }
}())::type;

template <BoundClass T, class Args, class Stored, std::size_t... Is>
[[nodiscard]] auto constructStored(Stored& stored, std::index_sequence<Is...>)
    -> std::shared_ptr<T> {
    return std::make_shared<ConstructedOf<T>>(
        ArgTraits<std::tuple_element_t<Is, Args>>::forward(std::get<Is>(stored))...);
}

template <BoundClass T, std::size_t I, Dispatch D>
bool tryConstruct(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx) {
    using CtorD = DescAt<T, I>;
    using Args = CtorD::ArgsTuple;
    []<class... A>(std::type_identity<std::tuple<A...>>) {
        if constexpr (HasTrampoline<T>) {
            static_assert(std::is_constructible_v<ConstructedOf<T>, A...>,
                          "Alka: ctor<Args...> does not match a constructor of the Trampoline "
                          "(inherit T's with `using T::T;`)");
        } else {
            static_assert(std::is_constructible_v<T, A...>,
                          "Alka: ctor<Args...> does not match any constructor of T");
        }
    }(std::type_identity<Args>{});
    const auto& site = kSite<T, I, CallSite::Kind::Construct>;
    if constexpr (isGcClass<T>) {
        return invokeWith<Args, D>(info, cx, site, descAt<T, I>(), [&](auto& stored) {
            guarded(cx.mIsolate, site, [&] {
                T* const object = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    return makeGcIn<T>(cx.mIsolate,
                                       ArgTraits<std::tuple_element_t<Is, Args>>::forward(
                                           std::get<Is>(stored))...);
                }(std::make_index_sequence<std::tuple_size_v<Args>>{});
                attachGcConstructed(cx, info.This(), object);
            });
        });
    } else {
        return invokeWith<Args, D>(info, cx, site, descAt<T, I>(), [&](auto& stored) {
            guarded(cx.mIsolate, site, [&] {
                auto object = constructStored<T, Args>(
                    stored, std::make_index_sequence<std::tuple_size_v<Args>>{});
                T* ptr = object.get();
                const void* const address = locateNative<T>(ptr).mKey;
                auto holder = Holder::owned(std::move(object), ptr);
                holder->mExternalBytes = externalSizeOf<T>(*ptr);
                attachWrapper(cx.mContext, info.This(), &kTypeNode<T>, std::move(holder), address);
            });
        });
    }
}

template <BoundClass T> void ctorCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    if (!info.IsConstructCall()) {
        throwTypeError(isolate, "Class constructor " + std::string(kTypeNode<T>.mName) +
                                    " cannot be invoked without 'new'");
        return;
    }
    using Members = MembersTupleOf<T>;
    constexpr auto ctors = kindIndicesOf<Members>(MemberKind::Ctor);
    constexpr std::size_t ctorCount = ctors.second;
    ConvertCtx cx{isolate, isolate->GetCurrentContext(), kDefaultInt64Mode, info.This()};
    if constexpr (ctorCount == 0) {
        throwTypeError(isolate, std::string(kTypeNode<T>.mName) +
                                    " cannot be constructed from JS (no ctor<> bound)");
    } else if constexpr (ctorCount == 1) {
        (void)tryConstruct<T, ctors.first[0], Dispatch::Single>(info, cx);
    } else {
        const bool handled = [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            static_assert((!anySpecDefault<DescAt<T, ctors.first[Js]>>() && ...),
                          "Alka: arg() defaults on constructor overload sets are ambiguous");
            return (tryConstruct<T, ctors.first[Js], Dispatch::Overload>(info, cx) || ...);
        }(std::make_index_sequence<ctorCount>{});
        if (!handled) {
            throwTypeError(isolate, "no constructor overload of " +
                                        std::string(kTypeNode<T>.mName) +
                                        " matches the given arguments " + describeArgs(info));
        }
    }
}

template <class T, class Desc> [[nodiscard]] consteval int jsLengthOf() {
    using F0 = std::remove_cvref_t<decltype(std::get<0>(Desc::callables))>;
    return static_cast<int>(minJsArity<Desc, CallArgsOfType<F0, T>>());
}

template <class T, class Owner, std::size_t I>
[[nodiscard]] auto memberFunctionTemplate(v8::Isolate* isolate, v8::FunctionCallback callback,
                                          v8::Local<v8::Signature> signature = {})
    -> v8::Local<v8::FunctionTemplate> {
    using Desc = DescAt<Owner, I>;
    const auto ft = v8::FunctionTemplate::New(
        isolate, callback, v8::Local<v8::Value>(), signature, jsLengthOf<T, Desc>(),
        v8::ConstructorBehavior::kThrow, v8::SideEffectType::kHasSideEffect,
        fastFunctionOf<T, Owner, I>());
    ft->SetClassName(v8Str(isolate, functionNameOf(Desc::name), true));
    return ft;
}

template <BoundClass T, std::size_t I>
void installProperty(v8::Isolate* isolate, const v8::Local<v8::FunctionTemplate> classFt,
                     const v8::Local<v8::Signature> signature) {
    using Desc = DescAt<T, I>;
    const auto getter =
        v8::FunctionTemplate::New(isolate, &getterCallback<T, I>, v8::Local<v8::Value>(), signature,
                                  0, v8::ConstructorBehavior::kThrow);
    v8::Local<v8::FunctionTemplate> setter;
    if constexpr (Desc::writable) {
        setter = v8::FunctionTemplate::New(isolate, &setterCallback<T, I>, v8::Local<v8::Value>(),
                                           signature, 1, v8::ConstructorBehavior::kThrow);
    }
    if (const auto key = memberKey(isolate, Desc::name); !key.IsEmpty()) {
        classFt->PrototypeTemplate()->SetAccessorProperty(key, getter, setter, v8::DontEnum);
    }
}

template <BoundClass T>
auto classTemplateOf(v8::Isolate* isolate) -> v8::Local<v8::FunctionTemplate> {
    static_assert(validateClassBinding<T>());
    auto& data = perIsolateData(isolate);
    if (const auto it = data.mClassTemplates.find(&kTypeNode<T>);
        it != data.mClassTemplates.end()) {
        return it->second.Get(isolate);
    }

    const auto ft = v8::FunctionTemplate::New(isolate, &ctorCallback<T>);
    ft->SetClassName(v8Str(isolate, kTypeNode<T>.mName, true));
    ft->InstanceTemplate()->SetInternalFieldCount(kInternalFieldCount);
    data.mClassTemplates.emplace(&kTypeNode<T>, v8::Global<v8::FunctionTemplate>(isolate, ft));
    registerKnownTypes<T>();

    if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        ft->Inherit(classTemplateOf<ExtendsOf<T>>(isolate));
    }

    const auto signature = v8::Signature::New(isolate, ft);
    forEachMemberIndexed<T>([&]<std::size_t I>(const auto&) {
        using Desc = DescAt<T, I>;
        if constexpr (Desc::kind == MemberKind::Ctor || Desc::kind == MemberKind::Var ||
                      Desc::kind == MemberKind::Indexer ||
                      Desc::kind == MemberKind::NamedProperties ||
                      Desc::kind == MemberKind::CallAsFunction) {
        } else if constexpr (Desc::kind == MemberKind::StaticProperty) {
            const auto getter = v8::FunctionTemplate::New(
                isolate, &staticGetterCallback<T, I>, v8::Local<v8::Value>(),
                v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow);
            v8::Local<v8::FunctionTemplate> setter;
            if constexpr (Desc::writable) {
                setter = v8::FunctionTemplate::New(
                    isolate, &staticSetterCallback<T, I>, v8::Local<v8::Value>(),
                    v8::Local<v8::Signature>(), 1, v8::ConstructorBehavior::kThrow);
            }
            if (const auto key = memberKey(isolate, Desc::name); !key.IsEmpty()) {
                ft->SetAccessorProperty(key, getter, setter, v8::DontEnum);
            }
        } else if constexpr (Desc::kind == MemberKind::Method) {
            if (const auto key = memberKey(isolate, Desc::name); !key.IsEmpty()) {
                ft->PrototypeTemplate()->Set(
                    key, memberFunctionTemplate<T, T, I>(isolate, &methodCallback<T, I>, signature),
                    v8::DontEnum);
            }
        } else if constexpr (Desc::kind == MemberKind::StaticMethod) {
            if (const auto key = memberKey(isolate, Desc::name); !key.IsEmpty()) {
                ft->Set(key, memberFunctionTemplate<void, T, I>(isolate, &functionCallback<T, I>),
                        v8::DontEnum);
            }
        } else if constexpr (Desc::kind == MemberKind::Property) {
            installProperty<T, I>(isolate, ft, signature);
        } else if constexpr (Desc::kind == MemberKind::Disposer) {
            static_assert(!isGcClass<T>,
                          "Alka: a C++-heap (GcObject) object is collected, not disposed");
            const auto dispose =
                v8::FunctionTemplate::New(isolate, &disposeCallback<T, I>, v8::Local<v8::Value>(),
                                          signature, 0, v8::ConstructorBehavior::kThrow);
            dispose->SetClassName(v8Str(isolate, Desc::name, true));
            ft->PrototypeTemplate()->Set(v8Str(isolate, Desc::name, true), dispose, v8::DontEnum);
            if (const auto key = memberKey(isolate, "@@dispose"); !key.IsEmpty()) {
                ft->PrototypeTemplate()->Set(key, dispose, v8::DontEnum);
            }
        } else {
            static_assert(AlwaysFalse<Desc>,
                          "Alka: this member kind is not valid inside a class binding");
        }
    });
    installInstanceHandlersOf<T>(ft);
    return ft;
}

template <BoundClass T>
void markNativeMethods(const v8::Local<v8::Context> context, const v8::Local<v8::Function> fn) {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Value> prototype;
    if (!fn->Get(context, cachedKey<"prototype">(isolate)).ToLocal(&prototype) ||
        !prototype->IsObject()) {
        return;
    }
    forEachMember<T>([&]<class D>(const D&) {
        if constexpr (D::kind == MemberKind::Method) {
            const auto key = memberKey(isolate, D::name);
            v8::Local<v8::Value> method;
            if (!key.IsEmpty() &&
                prototype.As<v8::Object>()->GetRealNamedProperty(context, key).ToLocal(&method) &&
                method->IsFunction() &&
                method.As<v8::Function>()->ScriptId() == v8::UnboundScript::kNoScriptId &&
                method.As<v8::Function>()->GetBoundFunction()->IsUndefined()) {
                (void)method.As<v8::Object>()->SetPrivate(
                    context, cachedPrivate<"Alka::nativeMethod">(isolate), v8::True(isolate));
            }
        }
    });
}

template <BoundClass T>
[[nodiscard]] auto classFunctionOf(v8::Local<v8::Context> context) -> v8::Local<v8::Function> {
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::Local<v8::Function> fn =
        classTemplateOf<T>(isolate)->GetFunction(context).ToLocalChecked();
    if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        setPrototypeOf(context, fn, classFunctionOf<ExtendsOf<T>>(context)).Check();
    }
    ConvertCtx cx{isolate, context};
    forEachMember<T>([&]<typename D>(const D& desc) {
        if constexpr (D::kind == MemberKind::Var) {
            if (const v8::Local<v8::String> key = v8Str(isolate, D::name, true);
                !fn->HasOwnProperty(context, key).FromMaybe(true)) {
                std::ignore = fn->DefineOwnProperty(
                    context, key, toJs(cx, desc.mValue),
                    static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete));
            }
        }
    });
    markNativeMethods<T>(context, fn);
    return fn;
}

template <class Mod, class E, class Desc>
void installEnum(v8::Local<v8::Context> context, const v8::Local<v8::Object> ns) {
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope scope(isolate);
    const auto object = v8::Object::New(isolate);
    constexpr auto attributes = static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);
    for (const auto& entry : jsEnumEntries<Mod, E>) {
        const auto raw =
            static_cast<long long>(static_cast<std::underlying_type_t<E>>(entry.mValue));
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

template <BoundModule Mod> void installModule(v8::Local<v8::Context> context);

template <BoundModule Mod>
bool markInstalled(const v8::Local<v8::Context> context, const v8::Local<v8::Object> target) {
    const auto isolate = v8::Isolate::GetCurrent();
    const auto key =
        cachedPrivate<FixedString{"Alka::installed "} + qualifiedTypeNameOf<Mod>>(isolate);
    if (target->HasPrivate(context, key).FromMaybe(false)) {
        return false;
    }
    std::ignore = target->SetPrivate(context, key, v8::True(isolate));
    return true;
}

template <BoundModule Mod>
void installMembers(v8::Local<v8::Context> context, v8::Local<v8::Object> ns,
                    const bool withSubmodules) {
    const auto isolate = v8::Isolate::GetCurrent();
    ConvertCtx cx{isolate, context};

    forEachMemberIndexed<Mod>([&]<std::size_t I>(const auto& desc) {
        using Desc = DescAt<Mod, I>;
        if constexpr (Desc::kind == MemberKind::Function) {
            const auto ft =
                memberFunctionTemplate<void, Mod, I>(isolate, &functionCallback<Mod, I>);
            defineNamespaceValue(context, ns, Desc::name, ft->GetFunction(context).ToLocalChecked(),
                                 true);
        } else if constexpr (Desc::kind == MemberKind::Var) {
            defineNamespaceValue(context, ns, Desc::name, toJs(cx, desc.mValue), true);
        } else if constexpr (Desc::kind == MemberKind::Submodule) {
            if (withSubmodules) {
                installModule<typename Desc::Module>(context);
            }
        } else if constexpr (Desc::kind == MemberKind::ClassRef) {
            using T = Desc::Type;
            static_assert(validateExport<T>());
            if constexpr (BoundClass<T>) {
                defineNamespaceValue(context, ns, kTypeNode<T>.mName, classFunctionOf<T>(context),
                                     true);
            }
        } else if constexpr (Desc::kind == MemberKind::EnumType) {
            installEnum<Mod, typename Desc::Type, Desc>(context, ns);
        } else if constexpr (Desc::kind == MemberKind::ErrorType) {
            registerError<typename Desc::Type, typename Desc::BaseType>(Desc::name);
            v8::Local<v8::Function> cls;
            if (errorClass(context, typeid(typename Desc::Type)).ToLocal(&cls)) {
                defineNamespaceValue(context, ns, Desc::name, cls, true);
            }
        } else {
            static_assert(AlwaysFalse<Desc>,
                          "Alka: this member kind is not valid inside a module binding");
        }
    });
}

template <BoundModule Mod> void installModule(v8::Local<v8::Context> context) {
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handleScope(isolate);
    if (const auto ns = ensureNamespace(context, modulePathOf<Mod>());
        markInstalled<Mod>(context, ns)) {
        installMembers<Mod>(context, ns, true);
    }
}

} // namespace Detail

template <BoundModule... Mods> void install(v8::Local<v8::Context> context) {
    static_assert(Detail::checkModulePack<false, Mods...>());
    (Detail::installModule<Mods>(context), ...);
}

template <BoundModule Mod>
void installInto(const v8::Local<v8::Context> context, const v8::Local<v8::Object> target) {
    static_assert(Detail::checkModulePack<false, Mod>());
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handleScope(isolate);
    if (!Detail::markInstalled<Mod>(context, target)) {
        return;
    }
    Detail::installMembers<Mod>(context, target, false);
    forEachMemberIndexed<Mod>([&]<std::size_t I>(const auto&) {
        using Desc = Detail::DescAt<Mod, I>;
        if constexpr (Desc::kind == MemberKind::Submodule) {
            using Sub = Desc::Module;
            constexpr auto path = modulePathOf<Sub>();
            const auto nested = v8::Object::New(isolate);
            Detail::defineNamespaceValue(context, target, path.substr(path.rfind('.') + 1), nested,
                                         true);
            installInto<Sub>(context, nested);
        }
    });
}

template <BoundClass... Ts> void registerClass(v8::Isolate* isolate) {
    (static_cast<void>(Detail::classTemplateOf<Ts>(isolate)), ...);
}

} // namespace Alka
