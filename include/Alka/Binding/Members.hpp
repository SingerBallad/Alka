#pragma once

#include <Alka/Binding/Meta.hpp>
#include <Alka/Binding/Naming.hpp>
#include <Alka/Core/FixedString.hpp>
#include <Alka/Core/NameOf.hpp>
#include <Alka/Core/Traits.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// Descriptor objects that make up a binding's member list:
//   static constexpr auto members = Alka::members(
//       Alka::ctor<std::string, int>(),
//       Alka::method<&Player::attack>(),             // auto-named "attack"
//       Alka::method<"restoreHp", &Player::heal>(),  // explicit name first
//       Alka::prop<&Player::mHp>());                 // auto-named "mHp", through the naming

namespace Alka {

enum class MemberKind : std::uint8_t {
    Ctor,
    Method,
    StaticMethod,
    Property,
    Function,
    Var,
    EnumType,
    Submodule,
    ClassRef,
    Indexer,
    ErrorType,
    Disposer,
    StaticProperty,
    NamedProperties,
    CallAsFunction,
};

namespace Detail {

template <class P> inline constexpr bool isConstFieldPointer = false;
template <class C, class M> inline constexpr bool isConstFieldPointer<M C::*> = std::is_const_v<M>;
template <class C, class M>
inline constexpr bool isConstFieldPointer<M C::* const> = std::is_const_v<M>;

template <class S> [[nodiscard]] consteval auto specPolicy() -> ReturnPolicy {
    if constexpr (IsPolicySpec<S>) {
        return S::value;
    } else {
        return ReturnPolicy::Automatic;
    }
}

template <class S> [[nodiscard]] consteval auto specInt64Mode() -> Int64Mode {
    if constexpr (IsInt64Spec<S>) {
        return S::value;
    } else {
        return kDefaultInt64Mode;
    }
}

template <class S> [[nodiscard]] consteval int specKeepAlive() {
    if constexpr (IsKeepAliveSpec<S>) {
        return S::index;
    } else {
        return -1;
    }
}

template <FixedString Name, class SpecTuple> struct DescBase {
    static constexpr std::string_view name = Name.view();
    static constexpr auto fixedName = Name;
    SpecTuple mSpecs;

    template <class F> constexpr void forEachSpec(F&& f) const {
        std::apply([&](const auto&... s) { (f(s), ...); }, mSpecs);
    }

    [[nodiscard]] constexpr auto docText() const -> std::string_view {
        std::string_view out{};
        forEachSpec([&]<typename T0>(const T0& s) {
            if constexpr (IsDocSpec<T0>) {
                out = s.mText;
            }
        });
        return out;
    }

    [[nodiscard]] constexpr bool readonlyFlag() const {
        auto out = false;
        forEachSpec([&]<typename T0>(const T0&) {
            if constexpr (IsReadonly<T0>) {
                out = true;
            }
        });
        return out;
    }

    [[nodiscard]] static consteval auto returnPolicy() -> ReturnPolicy {
        auto out = ReturnPolicy::Automatic;
        []<std::size_t... Is>(ReturnPolicy& o, std::index_sequence<Is...>) {
            ((IsPolicySpec<std::tuple_element_t<Is, SpecTuple>>
                  ? (void)(o = specPolicy<std::tuple_element_t<Is, SpecTuple>>())
                  : (void)0),
             ...);
        }(out, std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});
        return out;
    }

    [[nodiscard]] static consteval bool readonlySpec() {
        return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (IsReadonly<std::tuple_element_t<Is, SpecTuple>> || ... || false);
        }(std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});
    }

    [[nodiscard]] static consteval bool fastAllowed() {
        return ![]<std::size_t... Is>(std::index_sequence<Is...>) {
            return (IsNoFast<std::tuple_element_t<Is, SpecTuple>> || ... || false);
        }(std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});
    }

    [[nodiscard]] static consteval bool hasInt64Mode() {
        return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (IsInt64Spec<std::tuple_element_t<Is, SpecTuple>> || ... || false);
        }(std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});
    }

    [[nodiscard]] static consteval auto int64Mode() -> Int64Mode {
        auto out = kDefaultInt64Mode;
        []<std::size_t... Is>(Int64Mode& o, std::index_sequence<Is...>) {
            ((IsInt64Spec<std::tuple_element_t<Is, SpecTuple>>
                  ? (void)(o = specInt64Mode<std::tuple_element_t<Is, SpecTuple>>())
                  : (void)0),
             ...);
        }(out, std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});
        return out;
    }

    static constexpr std::size_t keepAliveCount =
        []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (std::size_t{IsKeepAliveSpec<std::tuple_element_t<Is, SpecTuple>> ? 1u : 0u} +
                    ... + std::size_t{0});
        }(std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});

    static constexpr auto keepAliveIndices = []<std::size_t... Is>(std::index_sequence<Is...>) {
        std::array<int, keepAliveCount> out{};
        std::size_t n = 0;
        ((IsKeepAliveSpec<std::tuple_element_t<Is, SpecTuple>>
              ? (void)(out[n++] = specKeepAlive<std::tuple_element_t<Is, SpecTuple>>())
              : (void)0),
         ...);
        return out;
    }(std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});

    [[nodiscard]] constexpr bool deprecatedFlag() const {
        auto out = false;
        forEachSpec([&]<typename T0>(const T0&) {
            if constexpr (IsDeprecated<T0>) {
                out = true;
            }
        });
        return out;
    }

    [[nodiscard]] constexpr auto deprecatedReason() const -> std::string_view {
        std::string_view out{};
        forEachSpec([&]<typename T0>(const T0& s) {
            if constexpr (IsDeprecated<T0>) {
                out = s.mReason;
            }
        });
        return out;
    }

    [[nodiscard]] constexpr auto tsTypeOverride() const -> std::string_view {
        std::string_view out{};
        forEachSpec([&]<typename T0>(const T0& s) {
            if constexpr (IsTsType<T0>) {
                out = s.mText;
            }
        });
        return out;
    }

    [[nodiscard]] constexpr auto argSpecCount() const -> std::size_t {
        std::size_t n = 0;
        forEachSpec([&]<typename T0>(const T0&) {
            if constexpr (IsArgSpec<T0>) {
                ++n;
            }
        });
        return n;
    }
};

[[nodiscard]] consteval bool validBindingName(const std::string_view name) {
    return isIdentifier(name);
}

} // namespace Detail

template <class SpecTuple, class... Args>
struct CtorDesc : Detail::DescBase<FixedString{"constructor"}, SpecTuple> {
    static constexpr auto kind = MemberKind::Ctor;
    using ArgsTuple = std::tuple<Args...>;
    static constexpr auto arity = sizeof...(Args);
};

template <FixedString Name, class SpecTuple, auto... Fs>
struct MethodDesc : Detail::DescBase<Name, SpecTuple> {
    template <FixedString N> using WithName = MethodDesc<N, SpecTuple, Fs...>;
    static constexpr auto kind = MemberKind::Method;
    static constexpr auto overloadCount = sizeof...(Fs);
    static constexpr auto callables = std::tuple{Fs...};
    static_assert(sizeof...(Fs) >= 1);
    static_assert((BindableCallableValue<decltype(Fs)> && ...),
                  "Alka: method<> expects member/function pointers or captureless lambdas");
};

template <FixedString Name, class SpecTuple, auto... Fs>
struct StaticMethodDesc : Detail::DescBase<Name, SpecTuple> {
    template <FixedString N> using WithName = StaticMethodDesc<N, SpecTuple, Fs...>;
    static constexpr auto kind = MemberKind::StaticMethod;
    static constexpr auto overloadCount = sizeof...(Fs);
    static constexpr auto callables = std::tuple{Fs...};
    static_assert(sizeof...(Fs) >= 1);
    static_assert((BindableCallableValue<decltype(Fs)> && ...),
                  "Alka: staticMethod<> expects function pointers or captureless lambdas");
    static_assert((!std::is_member_function_pointer_v<decltype(Fs)> && ...),
                  "Alka: staticMethod<> must not receive member function pointers");
};

template <FixedString Name, class SpecTuple, auto... Ps>
struct PropDesc : Detail::DescBase<Name, SpecTuple> {
    template <FixedString N> using WithName = PropDesc<N, SpecTuple, Ps...>;
    static constexpr auto kind = MemberKind::Property;
    static constexpr auto accessors = std::tuple{Ps...};
    static constexpr bool isField =
        sizeof...(Ps) == 1 && (std::is_member_object_pointer_v<decltype(Ps)> && ...);
    static constexpr bool hasSetter = isField || sizeof...(Ps) == 2;
    static constexpr bool writable = [] {
        if constexpr (!hasSetter || Detail::DescBase<Name, SpecTuple>::readonlySpec()) {
            return false;
        } else if constexpr (isField) {
            return !Detail::isConstFieldPointer<decltype(std::get<0>(std::tuple{Ps...}))>;
        } else {
            return true;
        }
    }();
    static_assert(sizeof...(Ps) >= 1 && sizeof...(Ps) <= 2,
                  "Alka: prop<> takes a field pointer, a getter, or a getter/setter pair");
    static_assert(isField || (BindableCallableValue<decltype(Ps)> && ...),
                  "Alka: prop<> accessors must be member/function pointers or captureless lambdas");
};

template <FixedString Name, class SpecTuple, auto... Fs>
struct FnDesc : Detail::DescBase<Name, SpecTuple> {
    template <FixedString N> using WithName = FnDesc<N, SpecTuple, Fs...>;
    static constexpr auto kind = MemberKind::Function;
    static constexpr auto overloadCount = sizeof...(Fs);
    static constexpr auto callables = std::tuple{Fs...};
    static_assert(sizeof...(Fs) >= 1);
    static_assert((BindableCallableValue<decltype(Fs)> && ...),
                  "Alka: fn<> expects function pointers or captureless lambdas");
    static_assert((!std::is_member_function_pointer_v<decltype(Fs)> && ...),
                  "Alka: fn<> binds free functions; use method<> inside a class binding");
};

template <FixedString Name, class SpecTuple, class V>
struct VarDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::Var;
    V mValue;
};

template <class SpecTuple, auto GetFn = nullptr, auto SetFn = nullptr, auto SizeFn = nullptr>
struct IndexerDesc : Detail::DescBase<FixedString{"[index]"}, SpecTuple> {
    static constexpr auto kind = MemberKind::Indexer;
    static constexpr auto getAccessor = GetFn;
    static constexpr auto setAccessor = SetFn;
    static constexpr auto sizeAccessor = SizeFn;
};

template <class P>
inline constexpr bool isStaticVariablePointer =
    std::is_pointer_v<P> && !std::is_function_v<std::remove_pointer_t<P>>;

template <FixedString Name, class SpecTuple, auto Get, auto Set = nullptr>
struct StaticPropDesc : Detail::DescBase<Name, SpecTuple> {
    template <FixedString N> using WithName = StaticPropDesc<N, SpecTuple, Get, Set>;
    static constexpr auto kind = MemberKind::StaticProperty;
    static constexpr auto getAccessor = Get;
    static constexpr auto setAccessor = Set;
    using GetType = std::remove_cv_t<decltype(Get)>;
    static constexpr bool hasSetter = !std::is_null_pointer_v<std::remove_cv_t<decltype(Set)>>;
    static constexpr bool isVariable = isStaticVariablePointer<GetType> && !hasSetter;
    static constexpr bool writable = [] {
        if constexpr (Detail::DescBase<Name, SpecTuple>::readonlySpec()) {
            return false;
        } else if constexpr (isVariable) {
            return !std::is_const_v<std::remove_pointer_t<GetType>>;
        } else {
            return hasSetter;
        }
    }();
    static_assert(
        isVariable ||
            (BindableCallableValue<GetType> && !std::is_member_function_pointer_v<GetType> &&
             (!hasSetter || (BindableCallableValue<std::remove_cv_t<decltype(Set)>> &&
                             !std::is_member_function_pointer_v<std::remove_cv_t<decltype(Set)>>))),
        "Alka: staticProp<> accessors must be free functions or captureless lambdas "
        "(or one pointer to a static variable)");
};

template <class SpecTuple, auto GetFn, auto SetFn = nullptr, auto HasFn = nullptr,
          auto RemoveFn = nullptr, auto KeysFn = nullptr>
struct NamedPropertiesDesc : Detail::DescBase<FixedString{"[name]"}, SpecTuple> {
    static constexpr auto kind = MemberKind::NamedProperties;
    static constexpr auto getAccessor = GetFn;
    static constexpr auto setAccessor = SetFn;
    static constexpr auto hasAccessor = HasFn;
    static constexpr auto removeAccessor = RemoveFn;
    static constexpr auto keysAccessor = KeysFn;
};

template <class SpecTuple, auto F>
struct CallAsFunctionDesc : Detail::DescBase<FixedString{"[call]"}, SpecTuple> {
    static constexpr auto kind = MemberKind::CallAsFunction;
    static constexpr auto overloadCount = 1;
    static constexpr auto callables = std::tuple{F};
    static_assert(BindableCallableValue<decltype(F)>,
                  "Alka: asFunction<> expects a member function or a callable taking the object "
                  "first");
};

template <FixedString Name, class SpecTuple, auto Before = nullptr>
struct DisposerDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::Disposer;
    static constexpr auto before = Before;
};

template <class E, FixedString Name, class SpecTuple>
struct EnumTypeDesc : Detail::DescBase<Name, SpecTuple> {
    template <FixedString N> using WithName = EnumTypeDesc<E, N, SpecTuple>;
    static constexpr auto kind = MemberKind::EnumType;
    using Type = E;
    static_assert(std::is_enum_v<E>, "Alka: enumType<> expects an enum type");
};

template <class Tag> struct SubmoduleDesc {
    static constexpr auto kind = MemberKind::Submodule;
    using Module = Tag;
};

template <class T> struct ClassRefDesc {
    static constexpr auto kind = MemberKind::ClassRef;
    using Type = T;
};

template <class T>
concept IsDescriptor = requires {
    { std::remove_cvref_t<T>::kind } -> std::convertible_to<MemberKind>;
};

template <class... Args, AnySpec... Specs> [[nodiscard]] constexpr auto ctor(Specs... specs) {
    return CtorDesc<std::tuple<Specs...>, Args...>{{std::tuple<Specs...>(specs...)}};
}

namespace Detail {

template <class D, NameKind Kind> struct Reflected : D {
    using AlkaUnnamed = D;
    static constexpr NameKind alkaNameKind = Kind;
};

template <NameKind Kind, class D> [[nodiscard]] constexpr auto reflected(const D desc) {
    return Reflected<D, Kind>{desc};
}

template <auto F> [[nodiscard]] consteval auto cxxNameOf() {
    constexpr auto name = valueNameOf<F>;
    static_assert(isIdentifier(name.view()),
                  "Alka: could not derive a name from this callable (a lambda, or a virtual member "
                  "function with MSVC); pass an explicit name, e.g. method<\"name\", ...>()");
    return name;
}

} // namespace Detail

template <auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto method(Specs... specs) {
    return Detail::reflected<NameKind::Method>(
        MethodDesc<Detail::cxxNameOf<F0>(), std::tuple<Specs...>, F0, Fs...>{
            {std::tuple<Specs...>(specs...)}});
}

template <FixedString Name, auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto method(Specs... specs) {
    return MethodDesc<Name, std::tuple<Specs...>, F0, Fs...>{{std::tuple<Specs...>(specs...)}};
}

template <auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto staticMethod(Specs... specs) {
    return Detail::reflected<NameKind::StaticMethod>(
        StaticMethodDesc<Detail::cxxNameOf<F0>(), std::tuple<Specs...>, F0, Fs...>{
            {std::tuple<Specs...>(specs...)}});
}

template <FixedString Name, auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto staticMethod(Specs... specs) {
    return StaticMethodDesc<Name, std::tuple<Specs...>, F0, Fs...>{
        {std::tuple<Specs...>(specs...)}};
}

template <auto P0, auto... Ps, AnySpec... Specs> [[nodiscard]] constexpr auto prop(Specs... specs) {
    return Detail::reflected<NameKind::Property>(
        PropDesc<Detail::cxxNameOf<P0>(), std::tuple<Specs...>, P0, Ps...>{
            {std::tuple<Specs...>(specs...)}});
}

template <FixedString Name, auto P0, auto... Ps, AnySpec... Specs>
[[nodiscard]] constexpr auto prop(Specs... specs) {
    return PropDesc<Name, std::tuple<Specs...>, P0, Ps...>{{std::tuple<Specs...>(specs...)}};
}

template <auto F0, auto... Fs, AnySpec... Specs> [[nodiscard]] constexpr auto fn(Specs... specs) {
    return Detail::reflected<NameKind::Function>(
        FnDesc<Detail::cxxNameOf<F0>(), std::tuple<Specs...>, F0, Fs...>{
            {std::tuple<Specs...>(specs...)}});
}

template <FixedString Name, auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto fn(Specs... specs) {
    return FnDesc<Name, std::tuple<Specs...>, F0, Fs...>{{std::tuple<Specs...>(specs...)}};
}

// A static property of the class (also reachable through bound subclasses):
//   Alka::staticProp<&Config::sDebug>()             Config.sDebug (reads and writes
//                                                   the variable; const: read-only)
//   Alka::staticProp<"version", &versionOf>()        Config.version (a getter)
//   Alka::staticProp<"level", &getLevel, &setLevel>()
template <auto Get, auto Set = nullptr, AnySpec... Specs>
[[nodiscard]] constexpr auto staticProp(Specs... specs) {
    return Detail::reflected<NameKind::StaticProperty>(
        StaticPropDesc<Detail::cxxNameOf<Get>(), std::tuple<Specs...>, Get, Set>{
            {std::tuple<Specs...>(specs...)}});
}

template <FixedString Name, auto Get, auto Set = nullptr, AnySpec... Specs>
[[nodiscard]] constexpr auto staticProp(Specs... specs) {
    return StaticPropDesc<Name, std::tuple<Specs...>, Get, Set>{{std::tuple<Specs...>(specs...)}};
}

template <FixedString Name, class V, AnySpec... Specs>
[[nodiscard]] constexpr auto var(V value, Specs... specs) {
    static_assert(Detail::validBindingName(Name.view()), "Alka: var<> name must be an identifier");
    return VarDesc<Name, std::tuple<Specs...>, V>{{std::tuple<Specs...>(specs...)}, value};
}

template <class E, AnySpec... Specs> [[nodiscard]] constexpr auto enumType(Specs... specs) {
    return Detail::reflected<NameKind::Type>(
        EnumTypeDesc<E, typeNameOf<E>, std::tuple<Specs...>>{{std::tuple<Specs...>(specs...)}});
}

template <FixedString Name, class E, AnySpec... Specs>
[[nodiscard]] constexpr auto enumType(Specs... specs) {
    return EnumTypeDesc<E, Name, std::tuple<Specs...>>{{std::tuple<Specs...>(specs...)}};
}

template <class T> inline constexpr ClassRefDesc<T> type{};

template <class Tag> inline constexpr SubmoduleDesc<Tag> submodule{};

template <class... Descs>
    requires(IsDescriptor<Descs> && ...)
[[nodiscard]] constexpr auto members(Descs... descs) {
    return std::tuple<Descs...>(descs...);
}

} // namespace Alka
