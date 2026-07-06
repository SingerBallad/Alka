#pragma once

#include <Alka/FixedString.hpp>
#include <Alka/Meta.hpp>
#include <Alka/NameOf.hpp>
#include <Alka/NamePolicy.hpp>
#include <Alka/Traits.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>

// Descriptor objects that make up a binding's member list:
//   static constexpr auto members = Alka::members(
//       Alka::ctor<std::string, int>(),
//       Alka::method<&Player::attack>(),          // auto-named "attack"
//       Alka::method<"restoreHp", &Player::heal>(),  // explicit name first
//       Alka::prop<&Player::mHp>());              // auto-named "hp"
// Everything is constexpr; callables travel as NTTPs so invokers can be
// synthesized from the descriptor *type* alone.

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
};

namespace Detail {

template<FixedString Name, class SpecTuple>
struct DescBase {
    static constexpr std::string_view name = Name.view();
    SpecTuple mSpecs;

    template<class F>
    constexpr void forEachSpec(F&& f) const {
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

}  // namespace Detail

template<class SpecTuple, class... Args>
struct CtorDesc : Detail::DescBase<FixedString{"constructor"}, SpecTuple> {
    static constexpr auto kind = MemberKind::Ctor;
    using ArgsTuple = std::tuple<Args...>;
    static constexpr auto arity = sizeof...(Args);
};

template<FixedString Name, class SpecTuple, auto... Fs>
struct MethodDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::Method;
    static constexpr auto overloadCount = sizeof...(Fs);
    static constexpr auto callables = std::tuple{Fs...};
    static_assert(sizeof...(Fs) >= 1);
    static_assert((BindableCallableValue<decltype(Fs)> && ...),
                  "Alka: method<> expects member/function pointers or captureless lambdas");
};

template<FixedString Name, class SpecTuple, auto... Fs>
struct StaticMethodDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::StaticMethod;
    static constexpr auto overloadCount = sizeof...(Fs);
    static constexpr auto callables = std::tuple{Fs...};
    static_assert(sizeof...(Fs) >= 1);
    static_assert((BindableCallableValue<decltype(Fs)> && ...),
                  "Alka: staticMethod<> expects function pointers or captureless lambdas");
    static_assert((!std::is_member_function_pointer_v<decltype(Fs)> && ...),
                  "Alka: staticMethod<> must not receive member function pointers");
};

template<FixedString Name, class SpecTuple, auto... Ps>
struct PropDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::Property;
    static constexpr auto accessors = std::tuple{Ps...};
    static constexpr bool isField =
        sizeof...(Ps) == 1 &&
        (std::is_member_object_pointer_v<decltype(Ps)> && ...);
    static constexpr bool hasSetter = isField || sizeof...(Ps) == 2;
    static_assert(sizeof...(Ps) >= 1 && sizeof...(Ps) <= 2,
                  "Alka: prop<> takes a field pointer, a getter, or a getter/setter pair");
    static_assert(isField || (BindableCallableValue<decltype(Ps)> && ...),
                  "Alka: prop<> accessors must be member/function pointers or captureless lambdas");
};

template<FixedString Name, class SpecTuple, auto... Fs>
struct FnDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::Function;
    static constexpr auto overloadCount = sizeof...(Fs);
    static constexpr auto callables = std::tuple{Fs...};
    static_assert(sizeof...(Fs) >= 1);
    static_assert((BindableCallableValue<decltype(Fs)> && ...),
                  "Alka: fn<> expects function pointers or captureless lambdas");
    static_assert((!std::is_member_function_pointer_v<decltype(Fs)> && ...),
                  "Alka: fn<> binds free functions; use method<> inside a class binding");
};

template<FixedString Name, class SpecTuple, class V>
struct VarDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::Var;
    V mValue;
};

template<class E, FixedString Name, class SpecTuple>
struct EnumTypeDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::EnumType;
    using Type = E;
    static_assert(std::is_enum_v<E>, "Alka: enumType<> expects an enum type");
};

template<class Tag>
struct SubmoduleDesc {
    static constexpr auto kind = MemberKind::Submodule;
    using Module = Tag;
};

template<class T>
struct ClassRefDesc {
    static constexpr auto kind = MemberKind::ClassRef;
    using Type = T;
};

template<class T>
concept IsDescriptor = requires {
    { std::remove_cvref_t<T>::kind } -> std::convertible_to<MemberKind>;
};

template<class... Args, AnySpec... Specs>
[[nodiscard]] constexpr auto ctor(Specs... specs) {
    return CtorDesc<std::tuple<Specs...>, Args...>{{std::tuple<Specs...>(specs...)}};
}

namespace Detail {

template<NameKind Kind, auto F0>
[[nodiscard]] consteval auto autoName() {
    constexpr auto name = transformedName<Kind, valueNameOf<F0>>;
    static_assert(isIdentifier(name.view()),
                  "Alka: could not derive a JS identifier from this callable; pass an explicit "
                  "name, e.g. method<\"name\", ...>()");
    return name;
}

}  // namespace Detail

template<auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto method(Specs... specs) {
    return MethodDesc<Detail::autoName<NameKind::Method, F0>(), std::tuple<Specs...>, F0, Fs...>{
        {std::tuple<Specs...>(specs...)}};
}

template<FixedString Name, auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto method(Specs... specs) {
    return MethodDesc<Name, std::tuple<Specs...>, F0, Fs...>{{std::tuple<Specs...>(specs...)}};
}

template<auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto staticMethod(Specs... specs) {
    return StaticMethodDesc<Detail::autoName<NameKind::StaticMethod, F0>(), std::tuple<Specs...>,
                            F0, Fs...>{{std::tuple<Specs...>(specs...)}};
}

template<FixedString Name, auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto staticMethod(Specs... specs) {
    return StaticMethodDesc<Name, std::tuple<Specs...>, F0, Fs...>{
        {std::tuple<Specs...>(specs...)}};
}

template<auto P0, auto... Ps, AnySpec... Specs>
[[nodiscard]] constexpr auto prop(Specs... specs) {
    return PropDesc<Detail::autoName<NameKind::Property, P0>(), std::tuple<Specs...>, P0, Ps...>{
        {std::tuple<Specs...>(specs...)}};
}

template<FixedString Name, auto P0, auto... Ps, AnySpec... Specs>
[[nodiscard]] constexpr auto prop(Specs... specs) {
    return PropDesc<Name, std::tuple<Specs...>, P0, Ps...>{{std::tuple<Specs...>(specs...)}};
}

template<auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto fn(Specs... specs) {
    return FnDesc<Detail::autoName<NameKind::Function, F0>(), std::tuple<Specs...>, F0, Fs...>{
        {std::tuple<Specs...>(specs...)}};
}

template<FixedString Name, auto F0, auto... Fs, AnySpec... Specs>
[[nodiscard]] constexpr auto fn(Specs... specs) {
    return FnDesc<Name, std::tuple<Specs...>, F0, Fs...>{{std::tuple<Specs...>(specs...)}};
}

template<FixedString Name, class V, AnySpec... Specs>
[[nodiscard]] constexpr auto var(V value, Specs... specs) {
    static_assert(Detail::validBindingName(Name.view()), "Alka: var<> name must be an identifier");
    return VarDesc<Name, std::tuple<Specs...>, V>{{std::tuple<Specs...>(specs...)}, value};
}

template<class E, AnySpec... Specs>
[[nodiscard]] constexpr auto enumType(Specs... specs) {
    constexpr auto name = Detail::transformedName<NameKind::Type, typeNameOf<E>>;
    static_assert(Detail::isIdentifier(name.view()),
                  "Alka: could not derive a name for this enum; use enumType<\"Name\", E>()");
    return EnumTypeDesc<E, name, std::tuple<Specs...>>{{std::tuple<Specs...>(specs...)}};
}

template<FixedString Name, class E, AnySpec... Specs>
[[nodiscard]] constexpr auto enumType(Specs... specs) {
    return EnumTypeDesc<E, Name, std::tuple<Specs...>>{{std::tuple<Specs...>(specs...)}};
}

template<class T>
inline constexpr ClassRefDesc<T> type{};

template<class Tag>
inline constexpr SubmoduleDesc<Tag> submodule{};

template<class... Descs>
    requires(IsDescriptor<Descs> && ...)
[[nodiscard]] constexpr auto members(Descs... descs) {
    return std::tuple<Descs...>(descs...);
}

}  // namespace Alka
