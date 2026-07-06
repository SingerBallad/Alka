#pragma once

#include <Alka/Converter.hpp>
#include <Alka/Error.hpp>
#include <Alka/Holder.hpp>
#include <Alka/Members.hpp>
#include <Alka/Traits.hpp>
#include <Alka/TypeNode.hpp>

#include <v8.h>

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace Alka::Detail {

inline constexpr auto kNodeField = 0;
inline constexpr auto kHolderField = 1;

struct CallSite {
    std::string mDisplayName;                // "math.add", "Player.attack", "new Player"
    std::vector<std::string_view> mArgNames; // from arg() specs; may be shorter than arity
    const void* mDescriptor = nullptr;       // the constexpr descriptor object (static storage)

    [[nodiscard]] auto argName(const std::size_t i) const noexcept -> std::string_view {
        return i < mArgNames.size() ? mArgNames[i] : std::string_view{};
    }
};

template <class Desc>
[[nodiscard]] auto makeCallSite(const std::string_view scope, const Desc& desc) -> CallSite {
    CallSite site;
    if (scope.empty()) {
        site.mDisplayName = std::string(Desc::name);
    } else {
        site.mDisplayName = std::string(scope) + "." + std::string(Desc::name);
    }
    site.mDescriptor = &desc;
    desc.forEachSpec([&]<typename T0>(const T0& s) {
        if constexpr (IsArgSpec<T0>) {
            site.mArgNames.push_back(s.mName);
        }
    });
    return site;
}

struct NoDesc {
    std::tuple<> mSpecs;
    template <class F> static constexpr void forEachSpec(F&&) {}
};
inline constexpr NoDesc kNoDesc{};

[[nodiscard]] inline auto v8Str(v8::Isolate* isolate, const std::string_view text,
                                const bool internalized = false) -> v8::Local<v8::String> {
    return v8::String::NewFromUtf8(isolate, text.data(),
                                   internalized ? v8::NewStringType::kInternalized
                                                : v8::NewStringType::kNormal,
                                   static_cast<int>(text.size()))
        .ToLocalChecked();
}

inline void throwTypeError(v8::Isolate* isolate, const std::string_view message) {
    isolate->ThrowException(v8::Exception::TypeError(v8Str(isolate, message)));
}

inline void throwError(v8::Isolate* isolate, const std::string_view message) {
    isolate->ThrowException(v8::Exception::Error(v8Str(isolate, message)));
}

template <BoundClass T>
[[nodiscard]] auto unwrapObjectWithHolder(const v8::Local<v8::Value> value)
    -> Result<std::pair<T*, Holder*>> {
    if (value.IsEmpty() || !value->IsObject()) {
        return std::unexpected(
            ConvertError::typeMismatch(kTypeNode<T>.mName, describeJsValue(value)));
    }
    const auto object = value.As<v8::Object>();
    if (object->InternalFieldCount() < 2) {
        return std::unexpected(ConvertError::typeMismatch(kTypeNode<T>.mName, "a plain JS object"));
    }
    const auto* node =
        static_cast<const TypeNode*>(object->GetAlignedPointerFromInternalField(kNodeField));
    auto* holder = static_cast<Holder*>(object->GetAlignedPointerFromInternalField(kHolderField));
    if (node == nullptr || holder == nullptr || holder->mPtr == nullptr) {
        return std::unexpected(ConvertError{std::string("the native ") +
                                            std::string(kTypeNode<T>.mName) +
                                            " object is not (or no longer) alive"});
    }
    void* adjusted = upcastAlong(node, &kTypeNode<T>, holder->mPtr);
    if (adjusted == nullptr) {
        return std::unexpected(ConvertError::typeMismatch(kTypeNode<T>.mName, node->mName));
    }
    return std::pair{static_cast<T*>(adjusted), holder};
}

template <BoundClass T>
[[nodiscard]] auto unwrapObject(const v8::Local<v8::Value> value) -> Result<T*> {
    auto unwrapped = unwrapObjectWithHolder<T>(value);
    if (!unwrapped) {
        return std::unexpected(std::move(unwrapped).error());
    }
    return unwrapped->first;
}

void registerTypeNodeIndex(const std::type_info& id, const TypeNode* node);

[[nodiscard]] auto lookupTypeNodeByTypeId(const std::type_info& id) -> const TypeNode*;

[[nodiscard]] auto wrapNative(const ConvertCtx& cx, const TypeNode* node, Holder* holder, void* cacheKey)
    -> v8::Local<v8::Object>;

[[nodiscard]] auto findCachedWrapper(ConvertCtx& cx, const TypeNode* node, const void* key)
    -> v8::Local<v8::Object>;

template <BoundClass T>
[[nodiscard]] auto mostDerivedNode(T* ptr) -> std::pair<const TypeNode*, void*> {
    if constexpr (std::is_polymorphic_v<T>) {
        if (auto node = lookupTypeNodeByTypeId(typeid(*ptr));
            node != nullptr && node != &kTypeNode<T>) {
            return {node, dynamic_cast<void*>(ptr)};
        }
        return {&kTypeNode<T>, dynamic_cast<void*>(const_cast<std::remove_const_t<T>*>(ptr))};
    } else {
        return {&kTypeNode<T>, const_cast<void*>(static_cast<const void*>(ptr))};
    }
}

template <class T> [[nodiscard]] auto wrapBorrowed(ConvertCtx& cx, T* ptr) -> v8::Local<v8::Value> {
    using U = std::remove_const_t<T>;
    const auto [node, address] = mostDerivedNode<U>(const_cast<U*>(ptr));
    if (const v8::Local<v8::Object> cached = findCachedWrapper(cx, node, address);
        !cached.IsEmpty()) {
        return cached;
    }
    return wrapNative(cx, node, new RefHolder(address), address);
}

template <class T>
[[nodiscard]] auto wrapOwned(ConvertCtx& cx, std::unique_ptr<T> owned) -> v8::Local<v8::Value> {
    const auto [node, address] = mostDerivedNode<T>(owned.get());
    auto* holder = new OwnedHolder<T>(std::move(owned));
    holder->mPtr = address;
    return wrapNative(cx, node, holder, address);
}

template <class T>
[[nodiscard]] auto wrapShared(ConvertCtx& cx, std::shared_ptr<T> shared) -> v8::Local<v8::Value> {
    const auto [node, address] = mostDerivedNode<T>(shared.get());
    if (const v8::Local<v8::Object> cached = findCachedWrapper(cx, node, address);
        !cached.IsEmpty()) {
        return cached;
    }
    auto* holder = new SharedHolder<T>(std::move(shared));
    holder->mPtr = address;
    return wrapNative(cx, node, holder, address);
}

template <class P>
inline constexpr bool isBoundClassRef =
    std::is_lvalue_reference_v<P> && BoundClass<std::remove_cvref_t<P>>;

template <class P>
inline constexpr bool isBoundClassPtr =
    std::is_pointer_v<std::remove_cvref_t<P>> &&
    BoundClass<std::remove_cv_t<std::remove_pointer_t<std::remove_cvref_t<P>>>>;

template <class Arg> struct ArgTraits {
    using Stored = std::remove_cvref_t<Arg>;

    static consteval void check() {
        static_assert(
            !(std::is_lvalue_reference_v<Arg> && !std::is_const_v<std::remove_reference_t<Arg>>),
            "Alka: non-const reference parameters are only supported for bound "
            "classes (JS values cannot alias primitives)");
        static_assert(FromJsConvertible<Stored>,
                      "Alka: no Converter<T>::fromJs for this parameter type; specialize "
                      "Alka::Converter or change the signature");
    }
    static auto convert(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<Stored> {
        return fromJs<Stored>(cx, value);
    }
    static auto forward(Stored& stored) -> Stored&& { return std::move(stored); }
};

template <class Arg>
    requires(isBoundClassRef<Arg>)
struct ArgTraits<Arg> {
    using T = std::remove_cvref_t<Arg>;
    using Stored = T*;

    static consteval void check() {}
    static auto convert(ConvertCtx&, v8::Local<v8::Value> value) -> Result<Stored> {
        return unwrapObject<T>(value);
    }
    static auto forward(Stored& stored) -> T& { return *stored; }
};

template <class Arg>
    requires(isBoundClassPtr<Arg>)
struct ArgTraits<Arg> {
    using T = std::remove_cv_t<std::remove_pointer_t<std::remove_cvref_t<Arg>>>;
    using Stored = T*;

    static consteval void check() {}
    static auto convert(ConvertCtx&, v8::Local<v8::Value> value) -> Result<Stored> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return nullptr;
        }
        return unwrapObject<T>(value);
    }
    static auto forward(const Stored& stored) -> T* { return stored; }
};

template <class Desc>
using SpecTupleOf = std::remove_cvref_t<decltype(std::declval<const Desc&>().mSpecs)>;

template <class SpecTuple> struct ArgSpecIndex {
    static constexpr auto kData = []<std::size_t... Is>(std::index_sequence<Is...>) {
        std::array<std::size_t, std::tuple_size_v<SpecTuple> + 1> indices{};
        std::size_t count = 0;
        ((IsArgSpec<std::tuple_element_t<Is, SpecTuple>> ? (indices[count++] = Is, 0) : 0), ...);
        return std::pair{indices, count};
    }(std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});

    static constexpr std::size_t count = kData.second;
    template <std::size_t J> static constexpr std::size_t tupleIndex = kData.first[J];
};

template <class Desc, std::size_t I, class Stored>
[[nodiscard]] std::optional<Stored> defaultArgValue(const Desc& desc) {
    using Index = ArgSpecIndex<SpecTupleOf<Desc>>;
    if constexpr (I < Index::count) {
        using SpecT = std::tuple_element_t<Index::template tupleIndex<I>, SpecTupleOf<Desc>>;
        if constexpr (SpecT::hasDefault) {
            static_assert(std::convertible_to<decltype(std::declval<SpecT>().mDefault), Stored>,
                          "Alka: this arg() default value is not convertible to the parameter "
                          "type");
            return static_cast<Stored>(
                std::get<Index::template tupleIndex<I>>(desc.mSpecs).mDefault);
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
}

template <class Desc, std::size_t I> [[nodiscard]] consteval bool hasDefaultFor() {
    using Index = ArgSpecIndex<SpecTupleOf<Desc>>;
    if constexpr (I < Index::count) {
        return std::tuple_element_t<Index::template tupleIndex<I>, SpecTupleOf<Desc>>::hasDefault;
    } else {
        return false;
    }
}

template <class Desc, std::size_t Arity> [[nodiscard]] consteval auto minArityOf() -> std::size_t {
    std::size_t minArity = Arity;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        auto trailing = true;
        ((trailing && hasDefaultFor<Desc, Arity - 1 - Is>() ? --minArity
                                                            : (trailing = false, minArity)),
         ...);
    }(std::make_index_sequence<Arity>{});
    return minArity;
}

template <class Desc> [[nodiscard]] consteval bool anySpecDefault() {
    auto any = false;
    [&]<std::size_t... Js>(std::index_sequence<Js...>) {
        ((any = any || hasDefaultFor<Desc, Js>()), ...);
    }(std::make_index_sequence<ArgSpecIndex<SpecTupleOf<Desc>>::count>{});
    return any;
}

struct ArgFailure {
    ConvertError mError;
    std::size_t mIndex = 0;
};

template <class ArgsTuple, class Desc, std::size_t... Is>
[[nodiscard]] auto tryConvertArguments(const v8::FunctionCallbackInfo<v8::Value>& info,
                                       ConvertCtx& cx, const Desc& desc, std::index_sequence<Is...>)
    -> std::expected<std::tuple<typename ArgTraits<std::tuple_element_t<Is, ArgsTuple>>::Stored...>,
                     ArgFailure> {
    (ArgTraits<std::tuple_element_t<Is, ArgsTuple>>::check(), ...);

    const auto convertOne = [&]<std::size_t I>() {
        using Traits = ArgTraits<std::tuple_element_t<I, ArgsTuple>>;
        const bool missing = I >= static_cast<std::size_t>(info.Length()) ||
                             info[static_cast<int>(I)]->IsUndefined();
        if (missing) {
            if (auto def = defaultArgValue<Desc, I, typename Traits::Stored>(desc)) {
                return Result<typename Traits::Stored>(std::move(*def));
            }
        }
        const auto value = I < static_cast<std::size_t>(info.Length())
                                               ? info[static_cast<int>(I)]
                                               : v8::Local<v8::Value>(v8::Undefined(cx.mIsolate));
        return Traits::convert(cx, value);
    };

    std::tuple<Result<typename ArgTraits<std::tuple_element_t<Is, ArgsTuple>>::Stored>...>
        converted{convertOne.template operator()<Is>()...};

    const ConvertError* firstError = nullptr;
    std::size_t errorIndex = 0;
    (void)((std::get<Is>(converted).has_value()
                ? false
                : (firstError = &std::get<Is>(converted).error(), errorIndex = Is, true)) ||
           ...);
    if (firstError != nullptr) {
        return std::unexpected(ArgFailure{*firstError, errorIndex});
    }
    return std::tuple{std::move(*std::get<Is>(converted))...};
}

template <class ArgsTuple, class Desc, std::size_t... Is>
[[nodiscard]] auto convertArguments(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                                    const CallSite& site, const Desc& desc,
                                    std::index_sequence<Is...> seq)
    -> std::optional<
        std::tuple<typename ArgTraits<std::tuple_element_t<Is, ArgsTuple>>::Stored...>> {
    auto converted = tryConvertArguments<ArgsTuple>(info, cx, desc, seq);
    if (!converted) {
        throwTypeError(
            cx.mIsolate,
            ConvertError::forArgument(std::move(converted.error().mError), converted.error().mIndex,
                                      site.argName(converted.error().mIndex), site.mDisplayName)
                .message());
        return std::nullopt;
    }
    return std::optional{std::move(*converted)};
}

[[nodiscard]] inline auto describeArgs(const v8::FunctionCallbackInfo<v8::Value>& info)
    -> std::string {
    std::string out = "(";
    for (auto i = 0; i < info.Length(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += describeJsValue(info[i]);
    }
    out += ")";
    return out;
}

template <class Fn> void guarded(v8::Isolate* isolate, const CallSite& site, Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const JsException& e) {
        throwError(isolate, e.message());
    } catch (const std::exception& e) {
        throwError(isolate, e.what());
    } catch (...) {
        throwError(isolate, "unknown C++ exception in " + site.mDisplayName);
    }
}

template <class X> inline constexpr auto isUniquePtrOfBound = false;
template <class T> inline constexpr bool isUniquePtrOfBound<std::unique_ptr<T>> = BoundClass<T>;

template <class Ret, class CallFn>
void applyAndReturn(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                    const CallSite& site, CallFn&& call) {
    guarded(cx.mIsolate, site, [&] {
        if constexpr (std::is_void_v<Ret>) {
            call();
        } else if constexpr (isUniquePtrOfBound<std::remove_cvref_t<Ret>>) {
            auto owned = call();
            if (!owned) {
                info.GetReturnValue().SetNull();
            } else {
                info.GetReturnValue().Set(wrapOwned(cx, std::move(owned)));
            }
        } else if constexpr (isBoundClassPtr<Ret>) {
            auto* ptr = call();
            if (ptr == nullptr) {
                info.GetReturnValue().SetNull();
            } else {
                info.GetReturnValue().Set(wrapBorrowed(cx, ptr));
            }
        } else if constexpr (isBoundClassRef<Ret>) {
            decltype(auto) ref = call();
            info.GetReturnValue().Set(wrapBorrowed(cx, std::addressof(ref)));
        } else if constexpr (BoundClass<std::remove_cvref_t<Ret>>) {
            using D = std::remove_cvref_t<Ret>;
            static_assert(std::move_constructible<D> || std::copy_constructible<D>,
                          "Alka: returning a bound class by value requires it to be movable "
                          "or copyable");
            info.GetReturnValue().Set(wrapOwned(cx, std::make_unique<D>(call())));
        } else {
            static_assert(ToJsConvertible<std::remove_cvref_t<Ret>>,
                          "Alka: no Converter<T>::toJs for this return type; specialize "
                          "Alka::Converter or change the signature");
            info.GetReturnValue().Set(toJs(cx, call()));
        }
    });
}

template <auto F, class ArgsTuple, class StoredTuple, std::size_t... Is>
decltype(auto) callStored(StoredTuple& stored, std::index_sequence<Is...>) {
    return F(ArgTraits<std::tuple_element_t<Is, ArgsTuple>>::forward(std::get<Is>(stored))...);
}

template <auto F, class Desc>
void invokeFree(const v8::FunctionCallbackInfo<v8::Value>& info, const CallSite& site,
                const Desc& desc) {
    using Traits = FnTraitsOf<F>;
    using ArgsTuple = Traits::ArgsTuple;
    const auto isolate = info.GetIsolate();
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};
    auto args = convertArguments<ArgsTuple>(info, cx, site, desc,
                                            std::make_index_sequence<Traits::arity>{});
    if (!args) {
        return;
    }
    applyAndReturn<typename Traits::Ret>(info, cx, site, [&]() -> decltype(auto) {
        return callStored<F, ArgsTuple>(*args, std::make_index_sequence<Traits::arity>{});
    });
}

template <auto F, class Desc = NoDesc>
void nativeCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    static_assert(!isMemberCallable<F>,
                  "Alka: member functions belong to class bindings, not modules");
    const auto* site = static_cast<const CallSite*>(info.Data().As<v8::External>()->Value());
    if constexpr (std::is_same_v<Desc, NoDesc>) {
        invokeFree<F>(info, *site, kNoDesc);
    } else {
        invokeFree<F>(info, *site, *static_cast<const Desc*>(site->mDescriptor));
    }
}

template <auto F, class Desc>
bool tryFreeOverload(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                     const CallSite& site, const Desc& desc) {
    using Traits = FnTraitsOf<F>;
    using ArgsTuple = Traits::ArgsTuple;
    const auto argc = static_cast<std::size_t>(info.Length());
    if (argc < minArityOf<Desc, Traits::arity>() || argc > Traits::arity) {
        return false;
    }
    auto args =
        tryConvertArguments<ArgsTuple>(info, cx, desc, std::make_index_sequence<Traits::arity>{});
    if (!args) {
        return false;
    }
    applyAndReturn<typename Traits::Ret>(info, cx, site, [&]() -> decltype(auto) {
        return callStored<F, ArgsTuple>(*args, std::make_index_sequence<Traits::arity>{});
    });
    return true;
}

template <class Desc> void freeOverloadCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    static_assert(!anySpecDefault<Desc>(),
                  "Alka: arg() defaults on an overload set are ambiguous; declare separate "
                  "single-callable bindings instead");
    const auto* site = static_cast<const CallSite*>(info.Data().As<v8::External>()->Value());
    const auto& desc = *static_cast<const Desc*>(site->mDescriptor);
    const auto isolate = info.GetIsolate();
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};

    const bool handled = [&]<std::size_t... Js>(std::index_sequence<Js...>) {
        return (tryFreeOverload<std::get<Js>(Desc::callables)>(info, cx, *site, desc) || ...);
    }(std::make_index_sequence<Desc::overloadCount>{});
    if (!handled) {
        throwTypeError(isolate, "no overload of " + site->mDisplayName +
                                    " matches the given arguments " + describeArgs(info));
    }
}

template <class Tuple, std::size_t... Is>
auto tailTupleProbe(std::index_sequence<Is...>)
    -> std::tuple<std::tuple_element_t<Is + 1, Tuple>...>;

template <class Tuple>
    requires(std::tuple_size_v<Tuple> >= 1)
using TailTuple =
    decltype(tailTupleProbe<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple> - 1>{}));

template <class SelfParam, class T> [[nodiscard]] decltype(auto) selfAs(T* self) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<SelfParam>>) {
        return self;
    } else {
        return *self;
    }
}

template <auto F, class ArgsTuple, class T, class StoredTuple, std::size_t... Is>
decltype(auto) callStoredOnSelf(T* self, StoredTuple& stored, std::index_sequence<Is...>) {
    if constexpr (std::is_member_function_pointer_v<decltype(F)>) {
        return (self->*F)(
            ArgTraits<std::tuple_element_t<Is, ArgsTuple>>::forward(std::get<Is>(stored))...);
    } else {
        using SelfParam = std::tuple_element_t<0, typename FnTraitsOf<F>::ArgsTuple>;
        return F(selfAs<SelfParam>(self),
                 ArgTraits<std::tuple_element_t<Is, ArgsTuple>>::forward(std::get<Is>(stored))...);
    }
}

template <auto F, class T> [[nodiscard]] consteval auto methodArgsProbe() {
    using Traits = FnTraitsOf<F>;
    if constexpr (std::is_member_function_pointer_v<decltype(F)>) {
        static_assert(std::is_base_of_v<typename Traits::Class, T>,
                      "Alka: this member function belongs to an unrelated class");
        return std::type_identity<typename Traits::ArgsTuple>{};
    } else {
        using FullArgs = Traits::ArgsTuple;
        static_assert(Traits::arity >= 1,
                      "Alka: a callable bound as a method must take the self object first");
        using SelfParam = std::tuple_element_t<0, FullArgs>;
        using SelfClass = std::remove_cv_t<std::remove_pointer_t<std::remove_cvref_t<SelfParam>>>;
        static_assert(std::is_base_of_v<SelfClass, T> || std::is_same_v<SelfClass, T>,
                      "Alka: the callable's first parameter must be the bound class (or a base)");
        return std::type_identity<TailTuple<FullArgs>>{};
    }
}

template <auto F, class T> using MethodArgsOf = decltype(methodArgsProbe<F, T>())::type;

template <class T, auto F, class Desc>
void invokeMethod(T* self, const v8::FunctionCallbackInfo<v8::Value>& info, const CallSite& site,
                  const Desc& desc) {
    const auto isolate = info.GetIsolate();
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};
    using Args = MethodArgsOf<F, T>;
    auto args = convertArguments<Args>(info, cx, site, desc,
                                       std::make_index_sequence<std::tuple_size_v<Args>>{});
    if (!args) {
        return;
    }
    applyAndReturn<typename FnTraitsOf<F>::Ret>(info, cx, site, [&]() -> decltype(auto) {
        return callStoredOnSelf<F, Args>(self, *args,
                                         std::make_index_sequence<std::tuple_size_v<Args>>{});
    });
}

template <class T, auto F, class Desc = NoDesc>
void methodCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto* site = static_cast<const CallSite*>(info.Data().As<v8::External>()->Value());
    auto self = unwrapObject<T>(info.This());
    if (!self) {
        throwTypeError(info.GetIsolate(), site->mDisplayName + ": " + self.error().message());
        return;
    }
    if constexpr (std::is_same_v<Desc, NoDesc>) {
        invokeMethod<T, F>(*self, info, *site, kNoDesc);
    } else {
        invokeMethod<T, F>(*self, info, *site, *static_cast<const Desc*>(site->mDescriptor));
    }
}

template <class T, auto F, class Desc>
bool tryMethodOverload(T* self, const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                       const CallSite& site, const Desc& desc) {
    using Args = MethodArgsOf<F, T>;
    constexpr std::size_t arity = std::tuple_size_v<Args>;
    const auto argc = static_cast<std::size_t>(info.Length());
    if (argc < minArityOf<Desc, arity>() || argc > arity) {
        return false;
    }
    auto args = tryConvertArguments<Args>(info, cx, desc, std::make_index_sequence<arity>{});
    if (!args) {
        return false;
    }
    applyAndReturn<typename FnTraitsOf<F>::Ret>(info, cx, site, [&]() -> decltype(auto) {
        return callStoredOnSelf<F, Args>(self, *args, std::make_index_sequence<arity>{});
    });
    return true;
}

template <class T, class Desc>
void methodOverloadCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    static_assert(!anySpecDefault<Desc>(),
                  "Alka: arg() defaults on an overload set are ambiguous; declare separate "
                  "single-callable bindings instead");
    const auto* site = static_cast<const CallSite*>(info.Data().As<v8::External>()->Value());
    auto self = unwrapObject<T>(info.This());
    if (!self) {
        throwTypeError(info.GetIsolate(), site->mDisplayName + ": " + self.error().message());
        return;
    }
    const auto& desc = *static_cast<const Desc*>(site->mDescriptor);
    const auto isolate = info.GetIsolate();
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};

    const bool handled = [&]<std::size_t... Js>(std::index_sequence<Js...>) {
        return (tryMethodOverload<T, std::get<Js>(Desc::callables)>(*self, info, cx, *site, desc) ||
                ...);
    }(std::make_index_sequence<Desc::overloadCount>{});
    if (!handled) {
        throwTypeError(isolate, "no overload of " + site->mDisplayName +
                                    " matches the given arguments " + describeArgs(info));
    }
}

template <class C, class M> auto memberClassProbe(M C::*) -> C;
template <class C, class M> auto memberTypeProbe(M C::*) -> M;

template <auto Field> using FieldClassOf = decltype(memberClassProbe(Field));
template <auto Field> using FieldTypeOf = decltype(memberTypeProbe(Field));

template <class T, auto Field>
void fieldGetterCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto* site = static_cast<const CallSite*>(info.Data().As<v8::External>()->Value());
    auto self = unwrapObject<T>(info.This());
    if (!self) {
        throwTypeError(info.GetIsolate(), site->mDisplayName + ": " + self.error().message());
        return;
    }
    const auto isolate = info.GetIsolate();
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};
    using FieldT = FieldTypeOf<Field>;
    if constexpr (BoundClass<std::remove_cv_t<FieldT>>) {
        applyAndReturn<FieldT&>(info, cx, *site, [&]() -> FieldT& { return (*self)->*Field; });
    } else {
        applyAndReturn<FieldT>(info, cx, *site,
                               [&]() -> decltype(auto) { return (*self)->*Field; });
    }
}

template <class T, auto Field>
void fieldSetterCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto* site = static_cast<const CallSite*>(info.Data().As<v8::External>()->Value());
    auto self = unwrapObject<T>(info.This());
    if (!self) {
        throwTypeError(info.GetIsolate(), site->mDisplayName + ": " + self.error().message());
        return;
    }
    auto isolate = info.GetIsolate();
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};
    using Value = std::remove_cvref_t<FieldTypeOf<Field>>;
    static_assert(FromJsConvertible<Value>,
                  "Alka: no Converter<T>::fromJs for this field type; bind it readonly or "
                  "specialize Alka::Converter");
    auto converted = fromJs<Value>(
        cx, info.Length() > 0 ? info[0] : v8::Local<v8::Value>(v8::Undefined(isolate)));
    if (!converted) {
        throwTypeError(isolate, site->mDisplayName + ": " + converted.error().message());
        return;
    }
    guarded(isolate, *site, [&] { (*self)->*Field = std::move(*converted); });
}

} // namespace Alka::Detail
