#pragma once

#include <Alka/Converter.hpp>
#include <Alka/NameOf.hpp>
#include <Alka/Traits.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace Alka {

template<>
struct Converter<bool> {
    static auto fromJs(const ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<bool> {
        if (!value->IsBoolean()) {
            return std::unexpected(ConvertError::typeMismatch("boolean", describeJsValue(value)));
        }
        return value->BooleanValue(cx.mIsolate);
    }
    static v8::Local<v8::Value> toJs(const ConvertCtx& cx, const bool value) {
        return v8::Boolean::New(cx.mIsolate, value);
    }
};

namespace Detail {

inline constexpr auto kMaxSafeInteger = 9007199254740991.0;  // 2^53 - 1

template<class T>
[[nodiscard]] consteval auto integerTypeName() -> std::string_view {
    if constexpr (std::is_signed_v<T>) {
        return sizeof(T) == 8 ? "int64" : sizeof(T) == 4 ? "int32" : sizeof(T) == 2 ? "int16" : "int8";
    } else {
        return sizeof(T) == 8   ? "uint64"
               : sizeof(T) == 4 ? "uint32"
               : sizeof(T) == 2 ? "uint16"
                                : "uint8";
    }
}

}  // namespace Detail

template<class T>
    requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>)
struct Converter<T> {
    static constexpr bool kIs64 = sizeof(T) == 8;
    static constexpr std::string_view kName = Detail::integerTypeName<T>();

    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<T> {
        if (value->IsBigInt()) {
            if constexpr (kIs64) {
                auto lossless = false;
                if constexpr (std::is_signed_v<T>) {
                    const std::int64_t x = value.As<v8::BigInt>()->Int64Value(&lossless);
                    if (lossless) {
                        return static_cast<T>(x);
                    }
                } else {
                    const std::uint64_t x = value.As<v8::BigInt>()->Uint64Value(&lossless);
                    if (lossless) {
                        return static_cast<T>(x);
                    }
                }
                return std::unexpected(ConvertError::outOfRange(kName, "this bigint"));
            } else {
                return std::unexpected(ConvertError::typeMismatch("number", "bigint"));
            }
        }
        if (!value->IsNumber()) {
            return std::unexpected(ConvertError::typeMismatch(kIs64 ? "number or bigint" : "number",
                                                              describeJsValue(value)));
        }
        const double d = value.As<v8::Number>()->Value();
        if (std::isnan(d) || std::isinf(d) || std::trunc(d) != d) {
            return std::unexpected(ConvertError::typeMismatch("integer", "non-integer number"));
        }
        if constexpr (kIs64) {
            if (d > Detail::kMaxSafeInteger || d < -Detail::kMaxSafeInteger) {
                return std::unexpected(
                    ConvertError{std::string(kName) +
                                 " beyond Number.MAX_SAFE_INTEGER precision; pass a bigint"});
            }
            if constexpr (std::is_unsigned_v<T>) {
                if (d < 0.0) {
                    return std::unexpected(ConvertError::outOfRange(kName, "a negative number"));
                }
            }
            return static_cast<T>(d);
        } else {
            constexpr auto lo = static_cast<double>((std::numeric_limits<T>::min)());
            constexpr auto hi = static_cast<double>((std::numeric_limits<T>::max)());
            if (d < lo || d > hi) {
                return std::unexpected(ConvertError::outOfRange(kName, "this number"));
            }
            return static_cast<T>(d);
        }
    }

    static auto toJs(const ConvertCtx& cx, T value) -> v8::Local<v8::Value> {
        if constexpr (kIs64) {
            if constexpr (std::is_signed_v<T>) {
                return v8::BigInt::New(cx.mIsolate, static_cast<std::int64_t>(value));
            } else {
                return v8::BigInt::NewFromUnsigned(cx.mIsolate, static_cast<std::uint64_t>(value));
            }
        } else if constexpr (std::is_signed_v<T>) {
            return v8::Integer::New(cx.mIsolate, static_cast<std::int32_t>(value));
        } else {
            return v8::Integer::NewFromUnsigned(cx.mIsolate, static_cast<std::uint32_t>(value));
        }
    }
};

template<class T>
    requires std::floating_point<T>
struct Converter<T> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<T> {
        if (!value->IsNumber()) {
            return std::unexpected(ConvertError::typeMismatch("number", describeJsValue(value)));
        }
        return static_cast<T>(value.As<v8::Number>()->Value());
    }
    static v8::Local<v8::Value> toJs(const ConvertCtx& cx, T value) {
        return v8::Number::New(cx.mIsolate, static_cast<double>(value));
    }
};

template<Enum E>
struct Converter<E> {
    static auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<E> {
        using U = std::underlying_type_t<E>;
        auto raw = Converter<std::conditional_t<sizeof(U) == 8, U, std::int32_t>>::fromJs(cx, value);
        if (!raw) {
            return std::unexpected(std::move(raw).error());
        }
        const E candidate = static_cast<E>(static_cast<U>(*raw));
        if constexpr (enumEntriesOf<E>.size() > 0) {
            if (enumNameOf(candidate).empty()) {
                return std::unexpected(ConvertError{
                    std::to_string(static_cast<long long>(*raw)) + " is not a valid " +
                    std::string(typeNameOf<E>.view()) + " value"});
            }
        }
        return candidate;
    }
    static auto toJs(const ConvertCtx& cx, E value) -> v8::Local<v8::Value> {
        using U = std::underlying_type_t<E>;
        return v8::Number::New(cx.mIsolate, static_cast<double>(static_cast<U>(value)));
    }
};

namespace Detail {

[[nodiscard]] inline v8::Local<v8::Value> utf8ToJs(const ConvertCtx& cx,
                                                   const std::string_view text) {
    return v8::String::NewFromUtf8(cx.mIsolate, text.data(), v8::NewStringType::kNormal,
                                   static_cast<int>(text.size()))
        .ToLocalChecked();
}

}  // namespace Detail

template<>
struct Converter<std::string> {
    static auto fromJs(const ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<std::string> {
        if (!value->IsString()) {
            return std::unexpected(ConvertError::typeMismatch("string", describeJsValue(value)));
        }
        const v8::String::Utf8Value utf8(cx.mIsolate, value);
        return std::string(*utf8, static_cast<std::size_t>(utf8.length()));
    }
    static auto toJs(const ConvertCtx& cx, const std::string& value) -> v8::Local<v8::Value> {
        return Detail::utf8ToJs(cx, value);
    }
};

template<>
struct Converter<std::string_view> {
    static auto toJs(const ConvertCtx& cx, const std::string_view value) -> v8::Local<v8::Value> {
        return Detail::utf8ToJs(cx, value);
    }
};

template<>
struct Converter<const char*> {
    static auto toJs(const ConvertCtx& cx, const char* value) -> v8::Local<v8::Value> {
        return Detail::utf8ToJs(cx, value != nullptr ? std::string_view(value) : std::string_view());
    }
};

}  // namespace Alka
