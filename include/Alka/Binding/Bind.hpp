#pragma once

#include <Alka/Binding/Members.hpp>
#include <Alka/Binding/Naming.hpp>
#include <Alka/Config.hpp>
#include <Alka/Core/NameOf.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
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
//   public:  // (Alka must see them: a private one is a compile error)
//       using AlkaExtends = Entity;
//       static constexpr auto alkaMembers = Alka::members(...);
//   };
//
// Both forms are equivalent; when both exist the Bind<> specialization wins.

namespace Alka {

template <class T> struct Bind;

template <class T, FixedString Name = FixedString<0>{}> struct Class {
    using AlkaBoundType = T;
    static constexpr std::string_view alkaClassName = Name.view();
    static constexpr auto alkaIsClass = true;
};

// A value type: converts to / from a plain JS object field by field (its
// members must be field props), with no wrapper or identity -- the .d.ts gets
// an `interface`.
//   template<> struct Alka::Bind<Vec2> : Alka::Record<Vec2> {
//       static constexpr auto members = Alka::members(
//           Alka::prop<&Vec2::x>(), Alka::prop<&Vec2::y>());
//   };
template <class T, FixedString Name = FixedString<0>{}> struct Record {
    using AlkaBoundType = T;
    static constexpr std::string_view alkaClassName = Name.view();
    static constexpr auto alkaIsRecord = true;
};

template <FixedString Path> struct Module {
    static constexpr std::string_view alkaModulePath = Path.view();
    static constexpr auto alkaIsModule = true;
};

namespace Detail {

template <class T>
concept HasBindSpec = requires { sizeof(Bind<T>); };

template <class T>
concept HasInClassMembers = requires { std::remove_cvref_t<T>::alkaMembers; };

} // namespace Detail

template <class T>
concept Bound = Detail::HasBindSpec<T> || Detail::HasInClassMembers<T>;

template <class T>
concept BoundModule = Detail::HasBindSpec<T> && requires { Bind<T>::alkaModulePath; };

template <class T>
concept BoundRecord = Detail::HasBindSpec<T> && requires { Bind<T>::alkaIsRecord; };

template <class T>
concept BoundClass = Bound<T> && !BoundModule<T> && !BoundRecord<T>;

namespace Detail {

template <class T> [[nodiscard]] consteval auto extendsProbe() {
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

} // namespace Detail

template <class T> using ExtendsOf = decltype(Detail::extendsProbe<T>())::type;

template <class T>
concept HasTrampoline = Detail::HasBindSpec<T> && requires { typename Bind<T>::Trampoline; };

// How T's that C++ lends JS (T* / T& results) stay valid:
// the declaration strict borrowing (ALKA_STRICT_BORROW) asks for, unless T is an:
// Alka::Trackable (which needs none):
//   template<> struct Alka::Bind<Entity> : Alka::Class<Entity> {
//       static constexpr auto lifetime = Alka::Lifetime::Manual;
// or in-class `static constexpr auto alkaLifetime = ...;`. Bound subclasses
// inherit it.
enum class Lifetime : std::uint8_t {
    Manual, // C++ calls Alka::invalidate(isolate, ptr) before destroying one JS may hold
    Static, // lent objects outlive the isolate (singletons, globals, arenas)
};

namespace Detail {

template <class T> [[nodiscard]] consteval bool declaresLifetime() {
    if constexpr (HasBindSpec<T> && requires { Bind<T>::lifetime; }) {
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(Bind<T>::lifetime)>, Lifetime>,
                      "Alka: Bind<T>::lifetime must be an Alka::Lifetime");
        return true;
    } else if constexpr (requires { T::alkaLifetime; }) {
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(T::alkaLifetime)>, Lifetime>,
                      "Alka: alkaLifetime must be an Alka::Lifetime");
        return true;
    } else if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        return declaresLifetime<ExtendsOf<T>>();
    } else {
        return false;
    }
}

} // namespace Detail

namespace Detail {

template <class T> [[nodiscard]] consteval const auto& namingOf() {
    if constexpr (HasBindSpec<T>) {
        if constexpr (requires { Bind<T>::naming; }) {
            static_assert(isNaming<std::remove_cv_t<decltype(Bind<T>::naming)>>,
                          "Alka: Bind<T>::naming must be an Alka::naming(...)");
            return Bind<T>::naming;
        } else if constexpr (!std::is_void_v<ExtendsOf<T>>) {
            return namingOf<ExtendsOf<T>>();
        } else {
            return kProjectNaming;
        }
    } else if constexpr (requires { T::alkaNaming; }) {
        static_assert(isNaming<std::remove_cv_t<decltype(T::alkaNaming)>>,
                      "Alka: alkaNaming must be an Alka::naming(...)");
        return T::alkaNaming;
    } else if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        return namingOf<ExtendsOf<T>>();
    } else {
        return kProjectNaming;
    }
}

template <NameKind Kind>
[[nodiscard]] consteval bool isReflectedJsName(const std::string_view name) {
    constexpr bool member = Kind == NameKind::Method || Kind == NameKind::StaticMethod ||
                            Kind == NameKind::Property || Kind == NameKind::StaticProperty;
    return isIdentifier(name) || (member && name.size() > 2 && name.starts_with("@@"));
}

template <class Owner, NameKind Kind, FixedString Cxx>
inline constexpr auto reflectedName = [] {
    constexpr NameText name = namingOf<Owner>().apply(Kind, Cxx.view());
    static_assert(isReflectedJsName<Kind>(name.view()),
                  "Alka: this C++ name (Cxx), through the binding's naming, is not a JS identifier "
                  "(a template instantiation, say): give the declaration an explicit name "
                  "(Class<T, \"Name\">, method<\"name\", ...>, ...) or the naming an Alka::rename");
    return FixedString<name.size()>::from(name.view());
}();

template <Bound T> [[nodiscard]] consteval const auto& declaredMembers() {
    if constexpr (HasBindSpec<T> && requires { Bind<T>::members; }) {
        return Bind<T>::members;
    } else if constexpr (HasInClassMembers<T>) {
        return T::alkaMembers;
    } else {
        static_assert(AlwaysFalse<T>, "Alka: a Bind<T> specialization must define "
                                      "`static constexpr auto members = Alka::members(...)`");
    }
}

template <class Owner, class D> [[nodiscard]] constexpr auto withJsName(const D& desc) {
    if constexpr (requires { D::alkaNameKind; }) {
        using Unnamed = typename D::AlkaUnnamed;
        using Named = typename Unnamed::template WithName<
            reflectedName<Owner, D::alkaNameKind, Unnamed::fixedName>>;
        return Named{{desc.mSpecs}};
    } else {
        return desc;
    }
}

template <Bound T>
inline constexpr auto resolvedMembers = std::apply(
    [](const auto&... descs) { return std::tuple{withJsName<T>(descs)...}; }, declaredMembers<T>());

} // namespace Detail

template <Bound T> [[nodiscard]] consteval auto boundNameOf() -> std::string_view {
    if constexpr (Detail::HasBindSpec<T> && requires { Bind<T>::alkaClassName; }) {
        if constexpr (!std::string_view{Bind<T>::alkaClassName}.empty()) {
            return std::string_view{Bind<T>::alkaClassName};
        } else {
            return Detail::reflectedName<T, NameKind::Type, typeNameOf<T>>.view();
        }
    } else if constexpr (requires { T::alkaClassName; }) {
        return std::string_view{T::alkaClassName};
    } else {
        return Detail::reflectedName<T, NameKind::Type, typeNameOf<T>>.view();
    }
}

template <Bound T> [[nodiscard]] consteval auto boundDocOf() -> std::string_view {
    if constexpr (Detail::HasBindSpec<T> && requires { Bind<T>::doc; }) {
        return std::string_view{Bind<T>::doc};
    } else if constexpr (requires { T::alkaDoc; }) {
        return std::string_view{T::alkaDoc};
    } else {
        return {};
    }
}

template <BoundModule T> [[nodiscard]] consteval auto modulePathOf() -> std::string_view {
    return Bind<T>::alkaModulePath;
}

namespace Detail {

enum class NameScope : std::uint8_t { None, Prototype, Constructor, Object };

template <class T> inline constexpr char kTypeTag = 0;

struct NameEntry {
    std::string_view mName;
    NameScope mScope = NameScope::None;
    const void* mValue = nullptr;
};

template <class Owner, class D>
[[nodiscard]] consteval auto nameEntriesOf() -> std::array<NameEntry, 2> {
    constexpr auto kind = D::kind;
    if constexpr (BoundModule<Owner>) {
        if constexpr (kind == MemberKind::ClassRef) {
            if constexpr (Bound<typename D::Type>) {
                return {NameEntry{.mName = boundNameOf<typename D::Type>(),
                                  .mScope = NameScope::Object,
                                  .mValue = &kTypeTag<typename D::Type>}};
            } else {
                return {};
            }
        } else if constexpr (kind == MemberKind::Submodule) {
            constexpr auto path = modulePathOf<typename D::Module>();
            return {NameEntry{.mName = path.substr(path.rfind('.') + 1),
                              .mScope = NameScope::Object,
                              .mValue = &kTypeTag<typename D::Module>}};
        } else if constexpr (kind == MemberKind::ErrorType) {
            return {NameEntry{.mName = D::name,
                              .mScope = NameScope::Object,
                              .mValue = &kTypeTag<typename D::Type>}};
        } else if constexpr (kind == MemberKind::Function || kind == MemberKind::Var ||
                             kind == MemberKind::EnumType) {
            return {NameEntry{.mName = D::name, .mScope = NameScope::Object}};
        } else {
            return {};
        }
    } else if constexpr (BoundRecord<Owner>) {
        return {NameEntry{.mName = D::name, .mScope = NameScope::Object}};
    } else if constexpr (kind == MemberKind::Method || kind == MemberKind::Property) {
        return {NameEntry{.mName = D::name, .mScope = NameScope::Prototype}};
    } else if constexpr (kind == MemberKind::Disposer) {
        return {NameEntry{.mName = D::name, .mScope = NameScope::Prototype},
                NameEntry{.mName = "@@dispose", .mScope = NameScope::Prototype}};
    } else if constexpr (kind == MemberKind::StaticMethod || kind == MemberKind::StaticProperty ||
                         kind == MemberKind::Var) {
        return {NameEntry{.mName = D::name, .mScope = NameScope::Constructor}};
    } else {
        return {};
    }
}

template <Bound T> [[nodiscard]] consteval auto memberNameEntries() {
    using Members = std::remove_cvref_t<decltype(resolvedMembers<T>)>;
    return []<std::size_t... Is>(std::index_sequence<Is...>) {
        const std::array<std::array<NameEntry, 2>, sizeof...(Is)> each{
            nameEntriesOf<T, std::tuple_element_t<Is, Members>>()...};
        std::array<NameEntry, 2 * sizeof...(Is)> out{};
        for (std::size_t i = 0; i < each.size(); ++i) {
            out[2 * i] = each[i][0];
            out[2 * i + 1] = each[i][1];
        }
        return out;
    }(std::make_index_sequence<std::tuple_size_v<Members>>{});
}

[[nodiscard]] consteval bool isReservedName(const NameEntry& entry) {
    if (entry.mScope == NameScope::Prototype) {
        return entry.mName == "constructor";
    }
    if (entry.mScope == NameScope::Constructor) {
        for (const std::string_view reserved :
             {"prototype", "name", "length", "arguments", "caller"}) {
            if (entry.mName == reserved) {
                return true;
            }
        }
    }
    return false;
}

struct NameClash {
    std::string_view mName; // empty: none
    bool mReserved = false;
};

template <std::size_t N>
[[nodiscard]] consteval auto findNameClash(const std::array<NameEntry, N>& entries) -> NameClash {
    for (std::size_t i = 0; i < N; ++i) {
        if (entries[i].mScope == NameScope::None) {
            continue;
        }
        if (isReservedName(entries[i])) {
            return {.mName = entries[i].mName, .mReserved = true};
        }
        for (std::size_t j = i + 1; j < N; ++j) {
            if (entries[j].mScope == entries[i].mScope && entries[j].mName == entries[i].mName) {
                return {.mName = entries[i].mName, .mReserved = false};
            }
        }
    }
    return {};
}

template <class Owner, class E>
inline constexpr auto jsEnumEntries = []<std::size_t... Ks>(std::index_sequence<Ks...>) {
    return std::array<EnumEntry<E>, sizeof...(Ks)>{EnumEntry<E>{
        enumEntriesOf<E>[Ks].mValue,
        reflectedName<Owner, NameKind::EnumValue,
                      enumValueNameStorage<E, static_cast<long long>(enumEntriesOf<E>[Ks].mValue)>>
            .view()}...};
}(std::make_index_sequence<enumEntriesOf<E>.size()>{});

template <class Owner, class E> [[nodiscard]] consteval auto enumNameEntries() {
    std::array<NameEntry, jsEnumEntries<Owner, E>.size()> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = {jsEnumEntries<Owner, E>[i].mName, NameScope::Object};
    }
    return out;
}

template <class Binding, FixedString JsName> consteval bool reportNameClash() {
    static_assert(AlwaysFalse<Binding>,
                  "Alka: two members of this binding get the same JS name (JsName): rename one -- "
                  "an explicit name, or an Alka::rename in the binding's naming");
    return false;
}

template <class Binding, FixedString JsName> consteval bool reportReservedName() {
    static_assert(AlwaysFalse<Binding>,
                  "Alka: a member's JS name (JsName) is one V8 uses itself -- constructor on "
                  "instances; prototype, name, length, arguments, caller on the class: rename it");
    return false;
}

template <class Enum, FixedString JsName> consteval bool reportEnumNameClash() {
    static_assert(AlwaysFalse<Enum>,
                  "Alka: two enumerators of this enum get the same JS name (JsName) through the "
                  "module's naming: add an Alka::rename to it");
    return false;
}

template <class Owner, class D> [[nodiscard]] consteval bool checkEnumNamesOf() {
    if constexpr (D::kind == MemberKind::EnumType) {
        if constexpr (constexpr NameClash clash =
                          findNameClash(enumNameEntries<Owner, typename D::Type>());
                      !clash.mName.empty()) {
            return reportEnumNameClash<typename D::Type,
                                       FixedString<clash.mName.size()>::from(clash.mName)>();
        }
    }
    return true;
}

template <Bound T> [[nodiscard]] consteval bool checkMemberNames() {
    constexpr NameClash clash = findNameClash(memberNameEntries<T>());
    if constexpr (clash.mName.empty()) {
        using Members = std::remove_cvref_t<decltype(resolvedMembers<T>)>;
        return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (checkEnumNamesOf<T, std::tuple_element_t<Is, Members>>() && ...);
        }(std::make_index_sequence<std::tuple_size_v<Members>>{});
    } else if constexpr (clash.mReserved) {
        return reportReservedName<T, FixedString<clash.mName.size()>::from(clash.mName)>();
    } else {
        return reportNameClash<T, FixedString<clash.mName.size()>::from(clash.mName)>();
    }
}

template <Bound T> [[nodiscard]] consteval const auto& bindingMembers() {
    static_assert(checkMemberNames<T>());
    return resolvedMembers<T>;
}

template <class T> [[nodiscard]] auto methodJsName(const std::string_view cxxName) -> NameText {
    return namingOf<T>().apply(NameKind::Method, cxxName);
}

} // namespace Detail

template <Bound T, class Visitor> constexpr void forEachMember(Visitor&& visit) {
    std::apply([&](const auto&... d) { (visit(d), ...); }, Detail::bindingMembers<T>());
}

template <Bound T>
using MembersTupleOf = std::remove_cvref_t<decltype(Detail::bindingMembers<T>())>;

template <Bound T> inline constexpr std::size_t memberCount = std::tuple_size_v<MembersTupleOf<T>>;

template <Bound T, class Visitor> constexpr void forEachMemberIndexed(Visitor&& visit) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (visit.template operator()<Is>(std::get<Is>(Detail::bindingMembers<T>())), ...);
    }(std::make_index_sequence<memberCount<T>>{});
}

namespace Detail {

struct ModuleName {
    std::string_view mPath;
    std::string_view mName;
    const void* mModule = nullptr;
    const void* mValue = nullptr;
};

template <class T, std::size_t... Ns>
[[nodiscard]] consteval auto concatArrays(const std::array<T, Ns>&... arrays) {
    std::array<T, (Ns + ... + 0)> out{};
    std::size_t n = 0;
    (
        [&] {
            for (const T& item : arrays) {
                out[n++] = item;
            }
        }(),
        ...);
    return out;
}

template <BoundModule Mod> [[nodiscard]] consteval auto moduleNames();

template <class D> [[nodiscard]] consteval auto submoduleNames() {
    if constexpr (D::kind == MemberKind::Submodule) {
        return moduleNames<typename D::Module>();
    } else {
        return std::array<ModuleName, 0>{};
    }
}

template <BoundModule Mod> [[nodiscard]] consteval auto ownModuleNames() {
    constexpr auto own = memberNameEntries<Mod>();
    std::array<ModuleName, own.size() + 1> self{};
    self[0] = {modulePathOf<Mod>(), {}, &kTypeTag<Mod>};
    for (std::size_t i = 0; i < own.size(); ++i) {
        if (own[i].mScope != NameScope::None) {
            self[i + 1] = {modulePathOf<Mod>(), own[i].mName, &kTypeTag<Mod>, own[i].mValue};
        }
    }
    return self;
}

template <BoundModule Mod, std::size_t... Is>
[[nodiscard]] consteval auto moduleNamesOf(std::index_sequence<Is...>) {
    return concatArrays(ownModuleNames<Mod>(),
                        submoduleNames<std::tuple_element_t<Is, MembersTupleOf<Mod>>>()...);
}

template <BoundModule Mod> [[nodiscard]] consteval auto moduleNames() {
    return moduleNamesOf<Mod>(std::make_index_sequence<memberCount<Mod>>{});
}

template <BoundModule... Mods>
[[nodiscard]] consteval auto findModuleClash(const bool pathsToo) -> ModuleName {
    const auto names = concatArrays(moduleNames<Mods>()...);
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            if (names[i].mModule == nullptr || names[j].mModule == nullptr ||
                names[i].mModule == names[j].mModule || names[i].mPath != names[j].mPath ||
                (names[i].mValue != nullptr && names[i].mValue == names[j].mValue)) {
                continue; // (a class or a submodule both export is one value)
            }
            if (names[i].mName.empty() ? pathsToo && names[j].mName.empty()
                                       : names[i].mName == names[j].mName) {
                return names[i];
            }
        }
    }
    return {};
}

template <FixedString Path, FixedString JsName> consteval bool reportModuleNameClash() {
    static_assert(AlwaysFalse<decltype(Path)>,
                  "Alka: two modules used together define the same JS name (JsName) in namespace "
                  "Path: rename one");
    return false;
}

template <FixedString Path> consteval bool reportModulePathClash() {
    static_assert(AlwaysFalse<decltype(Path)>,
                  "Alka: two modules registered together have the same path (Path), and an ES "
                  "module specifier names one module");
    return false;
}

template <bool PathsToo, BoundModule... Mods> [[nodiscard]] consteval bool checkModulePack() {
    if constexpr (constexpr ModuleName clash = findModuleClash<Mods...>(PathsToo);
                  clash.mModule == nullptr) {
        return true;
    } else if constexpr (clash.mName.empty()) {
        return reportModulePathClash<FixedString<clash.mPath.size()>::from(clash.mPath)>();
    } else {
        return reportModuleNameClash<FixedString<clash.mPath.size()>::from(clash.mPath),
                                     FixedString<clash.mName.size()>::from(clash.mName)>();
    }
}

} // namespace Detail

namespace Detail {

struct InClassNames {
    static constexpr int alkaMembers = 0;
    static constexpr int alkaClassName = 0;
    static constexpr int alkaDoc = 0;
    static constexpr int alkaLifetime = 0;
    static constexpr int alkaExternalSize = 0;
    static constexpr int alkaNaming = 0;
    using AlkaExtends = void;
};

template <class T> struct InClassNamesProbe : T, InClassNames {};

template<class T>
concept HidesInClassNames =
    std::is_class_v<T> && !HasBindSpec<T> && !std::is_final_v<T> &&
    ((!requires { InClassNamesProbe<T>::alkaMembers; } && !requires { T::alkaMembers; }) ||
     (!requires { typename InClassNamesProbe<T>::AlkaExtends; } &&
      !requires { typename T::AlkaExtends; }) ||
     (!requires { InClassNamesProbe<T>::alkaClassName; } && !requires { T::alkaClassName; }) ||
     (!requires { InClassNamesProbe<T>::alkaDoc; } && !requires { T::alkaDoc; }) ||
     (!requires { InClassNamesProbe<T>::alkaLifetime; } && !requires { T::alkaLifetime; }) ||
     (!requires { InClassNamesProbe<T>::alkaExternalSize; } &&
      !requires(const T& object) { object.alkaExternalSize(); }) ||
     (!requires { InClassNamesProbe<T>::alkaNaming; } && !requires { T::alkaNaming; }));

template <class T> [[nodiscard]] consteval bool checkInClassNames() {
    static_assert(!HidesInClassNames<T>,
                  "Alka: this class declares an in-class binding name (alkaMembers, AlkaExtends, "
                  "alkaClassName, alkaDoc, alkaLifetime, alkaExternalSize, alkaNaming) Alka cannot "
                  "see: make it public");
    return true;
}

} // namespace Detail

template <class T> [[nodiscard]] consteval bool validateExport() {
    static_assert(Detail::checkInClassNames<T>());
    static_assert(Bound<T>, "Alka: type<T> exports a type without a binding: specialize "
                            "Alka::Bind<T> (or give T a public alkaMembers)");
    return true;
}

template <Bound T> [[nodiscard]] consteval bool validateClassBinding() {
    static_assert(Detail::checkInClassNames<T>());
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

} // namespace Alka
