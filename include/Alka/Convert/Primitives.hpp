#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Core/NameOf.hpp>
#include <Alka/Core/Traits.hpp>
#include <Alka/Detail/Compat.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace Alka {

template <> struct Converter<bool> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<bool> {
        if (!value->IsBoolean()) {
            return std::unexpected(ConvertError::typeMismatch("boolean", describeJsValue(value)));
        }
        return value->BooleanValue(cx.mIsolate);
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const bool value) {
        return v8::Boolean::New(cx.mIsolate, value);
    }
};

namespace Detail {

inline constexpr auto kMaxSafeInteger = 9007199254740991.0; // 2^53 - 1

template <class T> [[nodiscard]] consteval auto integerTypeName() -> std::string_view {
    if constexpr (std::is_signed_v<T>) {
        return sizeof(T) == 8   ? "int64"
               : sizeof(T) == 4 ? "int32"
               : sizeof(T) == 2 ? "int16"
                                : "int8";
    } else {
        return sizeof(T) == 8   ? "uint64"
               : sizeof(T) == 4 ? "uint32"
               : sizeof(T) == 2 ? "uint16"
                                : "uint8";
    }
}

template <class T> [[nodiscard]] auto integerFromNumber(const double d) -> Result<T> {
    constexpr std::string_view name = integerTypeName<T>();
    const bool integral = d >= -0x1p63 && d < 0x1p63
                              ? static_cast<double>(static_cast<std::int64_t>(d)) == d
                              : !std::isnan(d) && !std::isinf(d);
    if (!integral) {
        return std::unexpected(ConvertError::typeMismatch("integer", "non-integer number"));
    }
    if constexpr (sizeof(T) == 8) {
        if (d > kMaxSafeInteger || d < -kMaxSafeInteger) {
            return std::unexpected(ConvertError{
                std::string(name) + " beyond Number.MAX_SAFE_INTEGER precision; pass a bigint"});
        }
        if constexpr (std::is_unsigned_v<T>) {
            if (d < 0.0) {
                return std::unexpected(ConvertError::outOfRange(name, "a negative number"));
            }
        }
        return static_cast<T>(d);
    } else {
        constexpr auto lo = static_cast<double>((std::numeric_limits<T>::min)());
        constexpr auto hi = static_cast<double>((std::numeric_limits<T>::max)());
        if (d < lo || d > hi) {
            return std::unexpected(ConvertError::outOfRange(name, "this number"));
        }
        return static_cast<T>(d);
    }
}

} // namespace Detail

template <class T>
    requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>)
struct Converter<T> {
    static constexpr bool kIs64 = sizeof(T) == 8;
    static constexpr std::string_view kName = Detail::integerTypeName<T>();

    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<T> {
        if constexpr (!kIs64) {
            if (value->IsInt32()) {
                const std::int32_t v = value.As<v8::Int32>()->Value();
                if constexpr (std::is_same_v<T, std::int32_t>) {
                    return v;
                } else if (std::in_range<T>(v)) {
                    return static_cast<T>(v);
                }
                return Detail::integerFromNumber<T>(static_cast<double>(v)); // (its range error)
            }
        }
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
        return Detail::integerFromNumber<T>(value.As<v8::Number>()->Value());
    }

    static auto toJs(ConvertCtx& cx, T value) -> v8::Local<v8::Value> {
        if constexpr (kIs64) {
            const bool exact = std::is_signed_v<T>
                                   ? (static_cast<std::int64_t>(value) <= 9007199254740991LL &&
                                      static_cast<std::int64_t>(value) >= -9007199254740991LL)
                                   : static_cast<std::uint64_t>(value) <= 9007199254740991ULL;
            if (cx.mInt64Mode == Int64Mode::Number || (cx.mInt64Mode == Int64Mode::Auto && exact)) {
                return v8::Number::New(cx.mIsolate, static_cast<double>(value));
            }
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

template <class T>
    requires std::floating_point<T>
struct Converter<T> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<T> {
        if (!value->IsNumber()) {
            return std::unexpected(ConvertError::typeMismatch("number", describeJsValue(value)));
        }
        return static_cast<T>(value.As<v8::Number>()->Value());
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, T value) {
        return v8::Number::New(cx.mIsolate, static_cast<double>(value));
    }
};

template <Enum E> struct Converter<E> {
    static auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<E> {
        using U = std::underlying_type_t<E>;
        auto raw =
            Converter<std::conditional_t<sizeof(U) == 8, U, std::int32_t>>::fromJs(cx, value);
        if (!raw) {
            return std::unexpected(std::move(raw).error());
        }
        const E candidate = static_cast<E>(static_cast<U>(*raw));
        if constexpr (enumEntriesOf<E>.size() > 0) {
            if (enumNameOf(candidate).empty()) {
                return std::unexpected(ConvertError{std::to_string(static_cast<long long>(*raw)) +
                                                    " is not a valid " +
                                                    std::string(typeNameOf<E>.view()) + " value"});
            }
        }
        return candidate;
    }
    static auto toJs(ConvertCtx& cx, E value) -> v8::Local<v8::Value> {
        using U = std::underlying_type_t<E>;
        return v8::Number::New(cx.mIsolate, static_cast<double>(static_cast<U>(value)));
    }
};

namespace Detail {

[[nodiscard]] inline v8::Local<v8::Value> utf8ToJs(ConvertCtx& cx, const std::string_view text) {
    v8::Local<v8::String> out;
    if (text.size() > static_cast<std::size_t>(v8::String::kMaxLength) ||
        !v8::String::NewFromUtf8(cx.mIsolate, text.data(), v8::NewStringType::kNormal,
                                 static_cast<int>(text.size()))
             .ToLocal(&out)) {
        throw JsException("string too long for a JS string");
    }
    return out;
}

} // namespace Detail

namespace Detail {

[[nodiscard]] inline auto utf8SizeOf(const std::uint8_t* chars, const std::size_t length)
    -> std::size_t {
    std::size_t size = length;
    for (std::size_t i = 0; i < length; ++i) {
        size += chars[i] >> 7; // Latin-1 >= 0x80 takes two bytes
    }
    return size;
}

inline auto encodeUtf8(const std::uint8_t* chars, const std::size_t length, char* dst) -> char* {
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = chars[i];
        if (c < 0x80) {
            *dst++ = static_cast<char>(c);
        } else {
            *dst++ = static_cast<char>(0xC0 | (c >> 6));
            *dst++ = static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return dst;
}

[[nodiscard]] inline auto codePointAt(const std::uint16_t* chars, const std::size_t length,
                                      std::size_t& i) -> char32_t {
    const char32_t c = chars[i];
    if (c < 0xD800 || c > 0xDFFF) {
        return c;
    }
    if (c <= 0xDBFF && i + 1 < length && chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
        return 0x10000 + ((c - 0xD800) << 10) + (chars[++i] - 0xDC00);
    }
    return 0xFFFD;
}

[[nodiscard]] inline auto utf8SizeOf(const std::uint16_t* chars, const std::size_t length)
    -> std::size_t {
    std::size_t size = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = codePointAt(chars, length, i);
        size += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    }
    return size;
}

inline auto encodeUtf8(const std::uint16_t* chars, const std::size_t length, char* dst) -> char* {
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = codePointAt(chars, length, i);
        if (c < 0x80) {
            *dst++ = static_cast<char>(c);
        } else if (c < 0x800) {
            *dst++ = static_cast<char>(0xC0 | (c >> 6));
            *dst++ = static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            *dst++ = static_cast<char>(0xE0 | (c >> 12));
            *dst++ = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            *dst++ = static_cast<char>(0x80 | (c & 0x3F));
        } else {
            *dst++ = static_cast<char>(0xF0 | (c >> 18));
            *dst++ = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            *dst++ = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            *dst++ = static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return dst;
}

[[nodiscard]] inline auto latin1ToUtf8(const std::uint8_t* chars, const std::size_t length)
    -> std::string {
    std::string out(utf8SizeOf(chars, length), '\0');
    (void)encodeUtf8(chars, length, out.data());
    return out;
}

[[nodiscard]] inline auto utf16ToUtf8(const std::uint16_t* chars, const std::size_t length)
    -> std::string {
    std::string out(utf8SizeOf(chars, length), '\0');
    (void)encodeUtf8(chars, length, out.data());
    return out;
}

class InlineUtf8 {
public:
    InlineUtf8() noexcept = default;
    InlineUtf8(InlineUtf8&& other) noexcept : mSize(other.mSize), mHeap(std::move(other.mHeap)) {
        if (mSize <= kInline) {
            std::memcpy(mInline, other.mInline, mSize);
        }
    }
    InlineUtf8(const InlineUtf8&) = delete;
    InlineUtf8& operator=(const InlineUtf8&) = delete;
    InlineUtf8& operator=(InlineUtf8&&) = delete;
    ~InlineUtf8() = default;

    template <class C> void assign(const C* chars, const std::size_t length) {
        if (length <= kInline / (sizeof(C) == 1 ? 2 : 3)) {
            mSize = static_cast<std::size_t>(encodeUtf8(chars, length, mInline) - mInline);
            return;
        }
        mSize = utf8SizeOf(chars, length);
        auto* dst = mInline;
        if (mSize > kInline) {
            mHeap.resize(mSize);
            dst = mHeap.data();
        }
        (void)encodeUtf8(chars, length, dst);
    }
    [[nodiscard]] auto view() const noexcept -> std::string_view {
        return {mSize <= kInline ? mInline : mHeap.data(), mSize};
    }

private:
    static constexpr std::size_t kInline = 120;
    std::size_t mSize = 0;
    std::string mHeap; // (past kInline)
    char mInline[kInline];
};

[[nodiscard]] inline auto stringToUtf8(v8::Isolate* isolate, const v8::Local<v8::String> string)
    -> std::string {
    return visitString(isolate, string, []<class C>(const C* chars, const std::size_t length) {
        if constexpr (sizeof(C) == 1) {
            return latin1ToUtf8(chars, length);
        } else {
            return utf16ToUtf8(chars, length);
        }
    });
}

} // namespace Detail

template <> struct Converter<std::string> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<std::string> {
        if (!value->IsString()) {
            return std::unexpected(ConvertError::typeMismatch("string", describeJsValue(value)));
        }
        return Detail::stringToUtf8(cx.mIsolate, value.As<v8::String>());
    }
    static auto toJs(ConvertCtx& cx, const std::string& value) -> v8::Local<v8::Value> {
        return Detail::utf8ToJs(cx, value);
    }
};

template <> struct Converter<std::string_view> {
    static auto toJs(ConvertCtx& cx, const std::string_view value) -> v8::Local<v8::Value> {
        return Detail::utf8ToJs(cx, value);
    }
};

template <> struct Converter<const char*> {
    static auto toJs(ConvertCtx& cx, const char* value) -> v8::Local<v8::Value> {
        return Detail::utf8ToJs(cx,
                                value != nullptr ? std::string_view(value) : std::string_view());
    }
};

namespace Detail {

[[nodiscard]] inline auto utf16ToJs(ConvertCtx& cx, const char16_t* data, const std::size_t size)
    -> v8::Local<v8::Value> {
    v8::Local<v8::String> out;
    if (size > static_cast<std::size_t>(v8::String::kMaxLength) ||
        !v8::String::NewFromTwoByte(cx.mIsolate, reinterpret_cast<const std::uint16_t*>(data),
                                    v8::NewStringType::kNormal, static_cast<int>(size))
             .ToLocal(&out)) {
        throw JsException("string too long for a JS string");
    }
    return out;
}

[[nodiscard]] inline auto utf16FromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
    -> Result<std::u16string> {
    if (value.IsEmpty() || !value->IsString()) {
        return std::unexpected(ConvertError::typeMismatch("string", describeJsValue(value)));
    }
    return visitString(cx.mIsolate, value.As<v8::String>(),
                       [](const auto* chars, const std::size_t length) {
                           std::u16string out(length, u'\0');
                           for (std::size_t i = 0; i < length; ++i) {
                               out[i] = static_cast<char16_t>(chars[i]);
                           }
                           return out;
                       });
}

} // namespace Detail

template <> struct Converter<std::u16string> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<std::u16string> {
        return Detail::utf16FromJs(cx, value);
    }
    static auto toJs(ConvertCtx& cx, const std::u16string& value) -> v8::Local<v8::Value> {
        return Detail::utf16ToJs(cx, value.data(), value.size());
    }
};

template <> struct Converter<std::u16string_view> {
    static auto toJs(ConvertCtx& cx, const std::u16string_view value) -> v8::Local<v8::Value> {
        return Detail::utf16ToJs(cx, value.data(), value.size());
    }
};

template <> struct Converter<std::wstring> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<std::wstring> {
        auto utf16 = Detail::utf16FromJs(cx, value);
        if (!utf16) {
            return std::unexpected(std::move(utf16).error());
        }
        if constexpr (sizeof(wchar_t) == sizeof(char16_t)) {
            return std::wstring(utf16->begin(), utf16->end());
        } else {
            std::wstring out;
            out.reserve(utf16->size());
            for (std::size_t i = 0; i < utf16->size(); ++i) {
                char32_t c = (*utf16)[i];
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < utf16->size()) {
                    const char32_t low = (*utf16)[i + 1];
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                        ++i;
                    }
                }
                out.push_back(static_cast<wchar_t>(c));
            }
            return out;
        }
    }
    static auto toJs(ConvertCtx& cx, const std::wstring& value) -> v8::Local<v8::Value> {
        if constexpr (sizeof(wchar_t) == sizeof(char16_t)) {
            return Detail::utf16ToJs(cx, reinterpret_cast<const char16_t*>(value.data()),
                                     value.size());
        } else {
            std::u16string utf16;
            utf16.reserve(value.size());
            for (const wchar_t wc : value) {
                const auto c = static_cast<char32_t>(wc);
                if (c >= 0x10000 && c <= 0x10FFFF) {
                    utf16.push_back(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
                    utf16.push_back(static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
                } else {
                    utf16.push_back(static_cast<char16_t>(c));
                }
            }
            return Detail::utf16ToJs(cx, utf16.data(), utf16.size());
        }
    }
};

// Paths travel as UTF-8 strings (never through the narrow ANSI code page).
template <> struct Converter<std::filesystem::path> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<std::filesystem::path> {
        auto text = Converter<std::string>::fromJs(cx, value);
        if (!text) {
            return std::unexpected(std::move(text).error());
        }
        return std::filesystem::path(std::u8string(text->begin(), text->end()));
    }
    static auto toJs(ConvertCtx& cx, const std::filesystem::path& value) -> v8::Local<v8::Value> {
        const std::u8string utf8 = value.u8string();
        return Detail::utf8ToJs(
            cx, std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    }
};

template <> struct Converter<std::monostate> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<std::monostate> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return std::monostate{};
        }
        return std::unexpected(ConvertError::typeMismatch("undefined", describeJsValue(value)));
    }
    static auto toJs(ConvertCtx& cx, std::monostate) -> v8::Local<v8::Value> {
        return v8::Undefined(cx.mIsolate);
    }
};

template <> struct Converter<std::nullptr_t> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<std::nullptr_t> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return nullptr;
        }
        return std::unexpected(ConvertError::typeMismatch("null", describeJsValue(value)));
    }
    static auto toJs(ConvertCtx& cx, std::nullptr_t) -> v8::Local<v8::Value> {
        return v8::Null(cx.mIsolate);
    }
};

} // namespace Alka
