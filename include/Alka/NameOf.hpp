#pragma once

#include <Alka/Config.hpp>
#include <Alka/FixedString.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Alka {

template<class E>
struct EnumRange {
    static constexpr auto min = ALKA_ENUM_RANGE_MIN;
    static constexpr auto max = ALKA_ENUM_RANGE_MAX;
};

namespace Detail {

template<class T>
[[nodiscard]] consteval std::string_view rawTypeSig() {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#else
#error "Alka NameOf: unsupported compiler"
#endif
}

template<auto V>
[[nodiscard]] consteval std::string_view rawValueSig() {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#else
#error "Alka NameOf: unsupported compiler"
#endif
}

[[nodiscard]] consteval auto extractPayload(std::string_view sig, const std::string_view marker,
                                            const std::string_view tail) -> std::string_view {
    const std::size_t open = sig.find(marker);
    if (open == std::string_view::npos) {
        return {};
    }
    const auto payload = sig.substr(open + marker.size());
    const std::size_t close = payload.rfind(tail);
    if (close == std::string_view::npos) {
        return {};
    }
    return payload.substr(0, close);
}

template<class T>
[[nodiscard]] consteval auto typePayload() -> std::string_view {
#if defined(__clang__) || defined(__GNUC__)
    return extractPayload(rawTypeSig<T>(), "[T = ", "]");
#else
    return extractPayload(rawTypeSig<T>(), "rawTypeSig<", ">(void)");
#endif
}

template<auto V>
[[nodiscard]] consteval auto valuePayload() -> std::string_view {
#if defined(__clang__) || defined(__GNUC__)
    return extractPayload(rawValueSig<V>(), "[V = ", "]");
#else
    return extractPayload(rawValueSig<V>(), "rawValueSig<", ">(void)");
#endif
}

[[nodiscard]] consteval bool isIdentifierChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '$';
}

[[nodiscard]] consteval bool isIdentifier(std::string_view s) {
    if (s.empty() || (s.front() >= '0' && s.front() <= '9')) {
        return false;
    }
    for (const char c : s) {
        if (!isIdentifierChar(c)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] consteval auto stripTypeKeywords(std::string_view s) -> std::string_view {
    for (const std::string_view kw : {"class ", "struct ", "enum ", "union "}) {
        if (s.starts_with(kw)) {
            return s.substr(kw.size());
        }
    }
    return s;
}

[[nodiscard]] consteval auto dropScope(std::string_view s) -> std::string_view {
    int depth = 0;
    for (std::size_t i = s.size(); i > 1; --i) {
        const char c = s[i - 1];
        if (c == '>') {
            ++depth;
        } else if (c == '<') {
            --depth;
        } else if (depth == 0 && c == ':' && s[i - 2] == ':') {
            return s.substr(i);
        }
    }
    return s;
}

// Parses the declaration name out of a rendered pointer/enum value:
//   MSVC : "&public: void __cdecl Player::attack(class Entity &)" -> "attack"
//   MSVC : "&class std::function<int (int)> __cdecl Ns::makeDoubler(void)"
//          -> "makeDoubler" (parens inside the return type must not confuse it)
//   MSVC : "{&InhTest::Npc::tag, 0}" (multi-base fat member pointer) -> "tag"
//   clang: "&GameTest::Player::attack"                            -> "attack"
//   both : "Color::Red" -> "Red", "&Player::mHp" -> "mHp", "&spawn" -> "spawn"
[[nodiscard]] consteval auto parseValueName(std::string_view payload) -> std::string_view {
    // 1) A function rendering ends with its parameter list; cut at the '('
    //    matching the LAST ')'. This survives parentheses nested anywhere in
    //    the return type or the parameters.
    if (const std::size_t lastClose = payload.rfind(')');
        lastClose != std::string_view::npos) {
        auto depth = 0;
        std::size_t open = std::string_view::npos;
        for (std::size_t i = lastClose + 1; i > 0; --i) {
            const char c = payload[i - 1];
            if (c == ')') {
                ++depth;
            } else if (c == '(') {
                if (--depth == 0) {
                    open = i - 1;
                    break;
                }
            }
        }
        if (open != std::string_view::npos) {
            payload = payload.substr(0, open);
        }
    }
    // 2) MSVC renders member pointers of classes with multiple/virtual bases
    //    as an aggregate "{&X::y, adj...}"; keep only the first element. The
    //    comma cut must only run in that braced form -- ordinary renderings
    //    contain commas inside return-type template arguments.
    if (!payload.empty() && payload.front() == '{') {
        payload.remove_prefix(1);
        if (const std::size_t comma = payload.find(','); comma != std::string_view::npos) {
            payload = payload.substr(0, comma);
        }
    }
    while (!payload.empty() && (payload.back() == '}' || payload.back() == ' ')) {
        payload.remove_suffix(1);
    }
    // 3) The declaration name is the trailing identifier run.
    std::size_t start = payload.size();
    while (start > 0 && isIdentifierChar(payload[start - 1])) {
        --start;
    }
    return payload.substr(start);
}

template<std::size_t N>
[[nodiscard]] consteval auto toFixed(std::string_view text) {
    return FixedString<N>::from(text);
}

template<class T>
inline constexpr auto qualifiedTypeNameStorage = [] {
    constexpr std::string_view name = stripTypeKeywords(typePayload<T>());
    static_assert(!name.empty(), "Alka NameOf: failed to parse the compiler's type rendering; "
                                 "please report the compiler version");
    return toFixed<name.size()>(name);
}();

template<class T>
inline constexpr auto typeNameStorage = [] {
    constexpr std::string_view name = dropScope(qualifiedTypeNameStorage<T>.view());
    return toFixed<name.size()>(name);
}();

template<auto V>
inline constexpr auto valueNameStorage = [] {
    constexpr std::string_view name = parseValueName(valuePayload<V>());
    static_assert(!name.empty(), "Alka NameOf: failed to parse the compiler's value rendering; "
                                 "pass an explicit name instead");
    return toFixed<name.size()>(name);
}();

}  // namespace Detail

// Unqualified type name: typeNameOf<Game::Player> == "Player". Template
// instantiations render with their argument list; bind such types with an
// explicit name instead.
template<class T>
inline constexpr auto typeNameOf = Detail::typeNameStorage<T>;

// Namespace-qualified type name, for diagnostics: "Game::Player".
template<class T>
inline constexpr auto qualifiedTypeNameOf = Detail::qualifiedTypeNameStorage<T>;

// Declaration name of a member pointer / function pointer / enum value:
//   valueNameOf<&Player::attack> == "attack",  valueNameOf<Color::Red> == "Red"
template<auto V>
inline constexpr auto valueNameOf = Detail::valueNameStorage<V>;

// True when the reflected name is a plain identifier (bindable without an
// explicit override).
template<class T>
[[nodiscard]] consteval bool hasIdentifierTypeName() {
    return Detail::isIdentifier(typeNameOf<T>.view());
}

template<class E>
    requires std::is_enum_v<E>
struct EnumEntry {
    E mValue{};
    std::string_view mName;
};

namespace Detail {

// A rendered value that is not a real enumerator shows up as a cast or a bare
// number: "(Color)7", "(enum Color)0x7", "7", "-1".
[[nodiscard]] consteval bool isEnumeratorRendering(std::string_view payload) {
    return !payload.empty() && payload.front() != '(' && payload.front() != '-' &&
           !(payload.front() >= '0' && payload.front() <= '9');
}

// Enums without a fixed underlying type cannot represent every value of their
// underlying type; casting outside the representable range is not a constant
// expression, which clang enforces. The requires-probe turns that into a
// clean "not an enumerator" instead of a hard error.
template<class E, long long I>
[[nodiscard]] consteval bool isValidEnumValue() {
    if constexpr (requires { valuePayload<static_cast<E>(I)>(); }) {
        return isEnumeratorRendering(valuePayload<static_cast<E>(I)>());
    } else {
        return false;
    }
}

// Range clamped to the underlying type so static_cast stays value-preserving.
template<class E>
[[nodiscard]] consteval auto clampedEnumRange() -> std::pair<long long, long long> {
    using U = std::underlying_type_t<E>;
    long long lo = EnumRange<E>::min;
    long long hi = EnumRange<E>::max;
    if constexpr (std::is_unsigned_v<U>) {
        if (lo < 0) {
            lo = 0;
        }
        if (const auto uMax = static_cast<unsigned long long>((std::numeric_limits<U>::max)());
            static_cast<unsigned long long>(hi) > uMax) {
            hi = static_cast<long long>(uMax);
        }
    } else {
        if (lo < static_cast<long long>((std::numeric_limits<U>::min)())) {
            lo = static_cast<long long>((std::numeric_limits<U>::min)());
        }
        if (hi > static_cast<long long>((std::numeric_limits<U>::max)())) {
            hi = static_cast<long long>((std::numeric_limits<U>::max)());
        }
    }
    return {lo, hi};
}

template<class E, long long I>
inline constexpr auto enumValueNameStorage = [] {
    constexpr std::string_view name = parseValueName(valuePayload<static_cast<E>(I)>());
    return toFixed<name.size()>(name);
}();

// Instantiation-safe view: invalid/unrepresentable values yield "" without
// ever forming the offending cast.
template<class E, long long I>
[[nodiscard]] consteval auto enumValueNameView() -> std::string_view {
    if constexpr (isValidEnumValue<E, I>()) {
        return enumValueNameStorage<E, I>.view();
    } else {
        return {};
    }
}

template<class E>
inline constexpr auto enumEntriesStorage = []<std::size_t... Is>(std::index_sequence<Is...>) {
    constexpr auto range = clampedEnumRange<E>();
    constexpr std::array<bool, sizeof...(Is)> valid{
        isValidEnumValue<E, range.first + static_cast<long long>(Is)>()...};
    constexpr std::array<std::string_view, sizeof...(Is)> names{
        enumValueNameView<E, range.first + static_cast<long long>(Is)>()...};
    constexpr std::size_t count = [&] {
        std::size_t n = 0;
        for (const bool v : valid) {
            n += v ? 1U : 0U;
        }
        return n;
    }();
    std::array<EnumEntry<E>, count> entries{};
    std::size_t out = 0;
    for (std::size_t i = 0; i < sizeof...(Is); ++i) {
        if (valid[i]) {
            entries[out].mValue = static_cast<E>(range.first + static_cast<long long>(i));
            entries[out].mName = names[i];
            ++out;
        }
    }
    return entries;
}(std::make_index_sequence<static_cast<std::size_t>(clampedEnumRange<E>().second -
                                                    clampedEnumRange<E>().first + 1)>{});

}  // namespace Detail

// All enumerators of E found in [EnumRange<E>::min, EnumRange<E>::max],
// in ascending value order.
template<class E>
    requires std::is_enum_v<E>
inline constexpr auto& enumEntriesOf = Detail::enumEntriesStorage<E>;

// Name of a runtime enum value ("" when the value is not a reflected
// enumerator).
template<class E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr auto enumNameOf(E value) noexcept -> std::string_view {
    for (const auto& entry : enumEntriesOf<E>) {
        if (entry.mValue == value) {
            return entry.mName;
        }
    }
    return {};
}

}  // namespace Alka
