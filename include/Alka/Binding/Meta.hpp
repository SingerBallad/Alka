#pragma once

#include <Alka/Binding/Policy.hpp>
#include <Alka/Core/FixedString.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

// Compile-time "spec" objects that decorate binding descriptors:
//   Alka::method<&Player::attack>(Alka::doc("attack a target"),
//                                 Alka::arg("target"),
//                                 Alka::meta<"category">("combat"))
// Specs are literal types stored in constexpr descriptor tuples.

namespace Alka {

struct MetaValue {
    enum class Kind : std::uint8_t { Bool, Int, Number, String };

    Kind mKind = Kind::Bool;
    bool mBool = false;
    long long mInt = 0;
    double mNumber = 0.0;
    std::string_view mString;

    constexpr MetaValue() = default;
    constexpr MetaValue(bool v) : mBool(v) {}
    constexpr MetaValue(std::integral auto v) : mKind(Kind::Int), mInt(static_cast<long long>(v)) {}
    constexpr MetaValue(std::floating_point auto v)
        : mKind(Kind::Number), mNumber(static_cast<double>(v)) {}
    constexpr MetaValue(std::string_view v) : mKind(Kind::String), mString(v) {}
    constexpr MetaValue(const char* v) : mKind(Kind::String), mString(v) {}
};

template <class Default = void> struct ArgSpec;

// arg("dt")            -> named parameter (for .d.ts and error messages)
// arg("dt") = 0.016    -> named parameter with a default value
// arg("dt").doc("...") -> parameter documentation
template <> struct ArgSpec<void> {
    std::string_view mName;
    std::string_view mDoc;

    template <class D> [[nodiscard]] constexpr ArgSpec<D> operator=(D value) const {
        return {mName, mDoc, value};
    }
    [[nodiscard]] constexpr auto doc(const std::string_view text) const -> ArgSpec {
        return {.mName = mName, .mDoc = text};
    }
    static constexpr auto hasDefault = false;
};

template <class Default> struct ArgSpec {
    std::string_view mName;
    std::string_view mDoc;
    Default mDefault{};

    [[nodiscard]] constexpr ArgSpec doc(const std::string_view text) const {
        return {.mName = mName, .mDoc = text, .mDefault = mDefault};
    }
    static constexpr auto hasDefault = true;
};

[[nodiscard]] constexpr auto arg(const std::string_view name) -> ArgSpec<> {
    return {.mName = name, .mDoc = {}};
}

struct DocSpec {
    std::string_view mText;
};
[[nodiscard]] constexpr auto doc(const std::string_view text) -> DocSpec {
    return {text};
}

struct ReadonlyTag {};
inline constexpr ReadonlyTag readonly{};

struct DeprecatedSpec {
    std::string_view mReason;
};
[[nodiscard]] constexpr auto deprecated(const std::string_view reason = {}) -> DeprecatedSpec {
    return {reason};
}

struct TsTypeSpec {
    std::string_view mText;
};

template <FixedString Text> inline constexpr TsTypeSpec tsType{Text.view()};

struct MetaKvSpec {
    std::string_view mKey;
    MetaValue mValue;
};

template <FixedString Key, class V>
[[nodiscard]] constexpr auto meta(const V& value) -> MetaKvSpec {
    return {.mKey = Key.view(), .mValue = MetaValue(value)};
}
[[nodiscard]] constexpr auto meta(const std::string_view key, const MetaValue& value)
    -> MetaKvSpec {
    return {.mKey = key, .mValue = value};
}

namespace Detail {

template <class T> inline constexpr auto isArgSpec = false;
template <class D> inline constexpr auto isArgSpec<ArgSpec<D>> = true;

} // namespace Detail

template <class T>
concept IsArgSpec = Detail::isArgSpec<std::remove_cvref_t<T>>;
template <class T>
concept IsDocSpec = std::same_as<std::remove_cvref_t<T>, DocSpec>;
template <class T>
concept IsReadonly = std::same_as<std::remove_cvref_t<T>, ReadonlyTag>;
template <class T>
concept IsDeprecated = std::same_as<std::remove_cvref_t<T>, DeprecatedSpec>;
template <class T>
concept IsTsType = std::same_as<std::remove_cvref_t<T>, TsTypeSpec>;
template <class T>
concept IsMetaKv = std::same_as<std::remove_cvref_t<T>, MetaKvSpec>;

template <class T>
concept AnySpec =
    IsArgSpec<T> || IsDocSpec<T> || IsReadonly<T> || IsDeprecated<T> || IsTsType<T> ||
    IsMetaKv<T> || IsPolicySpec<T> || IsKeepAliveSpec<T> || IsInt64Spec<T> || IsNoFast<T>;

} // namespace Alka
