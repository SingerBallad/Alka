#pragma once

#include <Alka/Fwd.hpp>
#include <Alka/Members.hpp>
#include <Alka/NameOf.hpp>
#include <Alka/NamePolicy.hpp>

#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// The single source of truth for a binding is the C++ type structure itself:
//
//   // non-intrusive form (also works for third-party types)
//   template<>
//   struct Alka::Bind<Player> : Alka::Class<Player> {
//       using Extends = Entity;                      // real C++ base class
//       static constexpr auto members = Alka::members(...);
//   };
//
//   // in-class form
//   class Player : public Entity {
//       ...
//       using AlkaExtends = Entity;
//       static constexpr auto alkaMembers = Alka::members(...);
//   };
//
// Both forms are equivalent; when both exist the Bind<> specialization wins.

namespace Alka {

template<class T, FixedString Name = Detail::transformedName<NameKind::Type, typeNameOf<T>>>
struct Class {
    using AlkaBoundType = T;
    static constexpr std::string_view alkaClassName = Name.view();
    static constexpr auto alkaIsClass = true;
};

template<FixedString Path>
struct Module {
    static constexpr std::string_view alkaModulePath = Path.view();
    static constexpr auto alkaIsModule = true;
};

namespace Detail {

template<class T>
concept HasBindSpec = requires { sizeof(Bind<T>); };

template<class T>
concept HasInClassMembers = requires { std::remove_cvref_t<T>::alkaMembers; };

}  // namespace Detail

template<class T>
concept Bound = Detail::HasBindSpec<T> || Detail::HasInClassMembers<T>;

template<class T>
concept BoundModule = Detail::HasBindSpec<T> && requires { Bind<T>::alkaModulePath; };

template<class T>
concept BoundClass = Bound<T> && !BoundModule<T>;

namespace Detail {

template<class T>
[[nodiscard]] consteval auto extendsProbe() {
    if constexpr (HasBindSpec<T>) {
        if constexpr (requires { typename Bind<T>::Extends; }) {
            return std::type_identity<typename Bind<T>::Extends>{};
        } else {
            return std::type_identity<void>{};
        }
    } else if constexpr (requires { typename T::AlkaExtends; }) {
        return std::type_identity<typename T::AlkaExtends>{};
    } else {
        return std::type_identity<void>{};
    }
}

}  // namespace Detail

template<class T>
using ExtendsOf = decltype(Detail::extendsProbe<T>())::type;

namespace Detail {

template<Bound T>
[[nodiscard]] consteval const auto& bindingMembers() {
    if constexpr (HasBindSpec<T> && requires { Bind<T>::members; }) {
        return (Bind<T>::members);
    } else if constexpr (HasInClassMembers<T>) {
        return (T::alkaMembers);
    } else {
        static_assert(AlwaysFalse<T>,
                      "Alka: a Bind<T> specialization must define "
                      "`static constexpr auto members = Alka::members(...)`");
    }
}

}  // namespace Detail

template<Bound T>
[[nodiscard]] consteval auto boundNameOf() -> std::string_view {
    if constexpr (Detail::HasBindSpec<T> && requires { Bind<T>::alkaClassName; }) {
        return std::string_view{Bind<T>::alkaClassName};
    } else if constexpr (requires { T::alkaClassName; }) {
        return std::string_view{T::alkaClassName};
    } else {
        return Detail::transformedName<NameKind::Type, typeNameOf<T>>.view();
    }
}

template<Bound T>
[[nodiscard]] consteval auto boundDocOf() -> std::string_view {
    if constexpr (Detail::HasBindSpec<T> && requires { Bind<T>::doc; }) {
        return std::string_view{Bind<T>::doc};
    } else if constexpr (requires { T::alkaDoc; }) {
        return std::string_view{T::alkaDoc};
    } else {
        return {};
    }
}

template<BoundModule T>
[[nodiscard]] consteval auto modulePathOf() -> std::string_view {
    return Bind<T>::alkaModulePath;
}

template<Bound T, class Visitor>
constexpr void forEachMember(Visitor&& visit) {
    std::apply([&](const auto&... d) { (visit(d), ...); }, Detail::bindingMembers<T>());
}

template<Bound T>
[[nodiscard]] consteval bool validateClassBinding() {
    using E = ExtendsOf<T>;
    if constexpr (!std::is_void_v<E>) {
        static_assert(std::is_base_of_v<E, T>,
                      "Alka: Extends/AlkaExtends must name a real C++ base class of T");
        static_assert(!std::is_same_v<E, T>, "Alka: a class cannot extend itself");
        static_assert(Bound<E>, "Alka: the base class must itself be bound");
    }
    static_assert(Detail::isIdentifier(boundNameOf<T>()),
                  "Alka: the JS class name must be an identifier; set one explicitly via "
                  "Alka::Class<T, \"Name\">");
    return true;
}

}  // namespace Alka
