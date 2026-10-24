#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Convert/Primitives.hpp>

#include <v8.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Alka::Detail {

enum class TypedKind : std::uint8_t {
    Int8,
    Uint8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    BigInt64,
    BigUint64,
    Float32,
    Float64,
    Other,
};

template <class T>
concept TypedElement =
    std::is_arithmetic_v<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
    !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> && !std::same_as<T, char16_t> &&
    !std::same_as<T, char32_t> && !std::same_as<T, unsigned char> &&
    (std::floating_point<T> ? (sizeof(T) == 4 || sizeof(T) == 8) : sizeof(T) <= 8);

template <TypedElement T> [[nodiscard]] consteval auto typedKindOf() -> TypedKind {
    if constexpr (std::floating_point<T>) {
        return sizeof(T) == 4 ? TypedKind::Float32 : TypedKind::Float64;
    } else if constexpr (std::is_signed_v<T>) {
        return sizeof(T) == 1   ? TypedKind::Int8
               : sizeof(T) == 2 ? TypedKind::Int16
               : sizeof(T) == 4 ? TypedKind::Int32
                                : TypedKind::BigInt64;
    } else {
        return sizeof(T) == 2   ? TypedKind::Uint16
               : sizeof(T) == 4 ? TypedKind::Uint32
                                : TypedKind::BigUint64;
    }
}

[[nodiscard]] constexpr auto typedArrayName(const TypedKind kind) -> std::string_view {
    switch (kind) {
    case TypedKind::Int8:
        return "Int8Array";
    case TypedKind::Uint8:
        return "Uint8Array";
    case TypedKind::Uint8Clamped:
        return "Uint8ClampedArray";
    case TypedKind::Int16:
        return "Int16Array";
    case TypedKind::Uint16:
        return "Uint16Array";
    case TypedKind::Int32:
        return "Int32Array";
    case TypedKind::Uint32:
        return "Uint32Array";
    case TypedKind::BigInt64:
        return "BigInt64Array";
    case TypedKind::BigUint64:
        return "BigUint64Array";
    case TypedKind::Float32:
        return "Float32Array";
    case TypedKind::Float64:
        return "Float64Array";
    case TypedKind::Other:
        break;
    }
    return "TypedArray";
}

[[nodiscard]] inline auto typedKindOfValue(const v8::Local<v8::Value> value) -> TypedKind {
    if (value.IsEmpty() || !value->IsTypedArray()) {
        return TypedKind::Other;
    }
    if (value->IsFloat64Array()) {
        return TypedKind::Float64;
    }
    if (value->IsFloat32Array()) {
        return TypedKind::Float32;
    }
    if (value->IsInt32Array()) {
        return TypedKind::Int32;
    }
    if (value->IsUint32Array()) {
        return TypedKind::Uint32;
    }
    if (value->IsInt16Array()) {
        return TypedKind::Int16;
    }
    if (value->IsUint16Array()) {
        return TypedKind::Uint16;
    }
    if (value->IsInt8Array()) {
        return TypedKind::Int8;
    }
    if (value->IsUint8Array()) {
        return TypedKind::Uint8;
    }
    if (value->IsUint8ClampedArray()) {
        return TypedKind::Uint8Clamped;
    }
    if (value->IsBigInt64Array()) {
        return TypedKind::BigInt64;
    }
    if (value->IsBigUint64Array()) {
        return TypedKind::BigUint64;
    }
    return TypedKind::Other; // (e.g. Float16Array)
}

template <class T, class S> [[nodiscard]] auto typedElementAs(const S value) -> Result<T> {
    if constexpr (std::integral<S> && sizeof(S) == 8) {
        if constexpr (std::integral<T> && sizeof(T) == 8) {
            if constexpr (std::is_signed_v<T> == std::is_signed_v<S>) {
                return static_cast<T>(value);
            } else if constexpr (std::is_signed_v<T>) {
                if (value > static_cast<S>((std::numeric_limits<T>::max)())) {
                    return std::unexpected(ConvertError::outOfRange("int64", "this bigint"));
                }
                return static_cast<T>(value);
            } else {
                if (value < 0) {
                    return std::unexpected(ConvertError::outOfRange("uint64", "this bigint"));
                }
                return static_cast<T>(value);
            }
        } else {
            return std::unexpected(ConvertError::typeMismatch("number", "bigint"));
        }
    } else if constexpr (std::floating_point<T>) {
        return static_cast<T>(value);
    } else {
        return integerFromNumber<T>(static_cast<double>(value));
    }
}

template <class Visit>
decltype(auto) visitTypedArray(const v8::Local<v8::TypedArray> array, const TypedKind kind,
                               Visit&& visit) {
    const auto copy = [&]<class S>(std::type_identity<S>) {
        std::vector<S> elements(array->Length());
        if (!elements.empty()) {
            array->CopyContents(elements.data(), elements.size() * sizeof(S));
        }
        return visit(static_cast<const S*>(elements.data()), elements.size());
    };
    switch (kind) {
    case TypedKind::Int8:
        return copy(std::type_identity<std::int8_t>{});
    case TypedKind::Uint8:
    case TypedKind::Uint8Clamped:
        return copy(std::type_identity<std::uint8_t>{});
    case TypedKind::Int16:
        return copy(std::type_identity<std::int16_t>{});
    case TypedKind::Uint16:
        return copy(std::type_identity<std::uint16_t>{});
    case TypedKind::Int32:
        return copy(std::type_identity<std::int32_t>{});
    case TypedKind::Uint32:
        return copy(std::type_identity<std::uint32_t>{});
    case TypedKind::BigInt64:
        return copy(std::type_identity<std::int64_t>{});
    case TypedKind::BigUint64:
        return copy(std::type_identity<std::uint64_t>{});
    case TypedKind::Float32:
        return copy(std::type_identity<float>{});
    case TypedKind::Float64:
    case TypedKind::Other:
        break;
    }
    return copy(std::type_identity<double>{});
}

template <TypedElement T, class Alloc = std::allocator<T>>
[[nodiscard]] auto typedArrayToVector(const v8::Local<v8::Value> value)
    -> Result<std::vector<T, Alloc>> {
    const auto kind = typedKindOfValue(value);
    if (kind == TypedKind::Other) {
        return std::unexpected(
            ConvertError::typeMismatch(typedArrayName(typedKindOf<T>()), describeJsValue(value)));
    }
    const auto array = value.As<v8::TypedArray>();
    if (kind == typedKindOf<T>()) {
        std::vector<T, Alloc> out(array->Length());
        if (!out.empty()) {
            array->CopyContents(out.data(), out.size() * sizeof(T));
        }
        return out;
    }
    return visitTypedArray(
        array, kind,
        [](const auto* elements, const std::size_t count) -> Result<std::vector<T, Alloc>> {
            std::vector<T, Alloc> out;
            out.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                auto element = typedElementAs<T>(elements[i]);
                if (!element) {
                    return std::unexpected(ConvertError::forElement(std::move(element).error(), i));
                }
                out.push_back(*element);
            }
            return out;
        });
}

[[nodiscard]] inline auto byteMemory(const v8::Local<v8::Value> value, std::size_t& offset,
                                     std::size_t& length) -> std::shared_ptr<v8::BackingStore> {
    std::shared_ptr<v8::BackingStore> store;
    if (!value.IsEmpty() && value->IsArrayBufferView()) {
        const auto view = value.As<v8::ArrayBufferView>();
        store = view->Buffer()->GetBackingStore();
        offset = view->ByteOffset();
        length = view->ByteLength();
    } else if (!value.IsEmpty() && value->IsArrayBuffer()) {
        const auto buffer = value.As<v8::ArrayBuffer>();
        store = buffer->GetBackingStore();
        offset = 0;
        length = buffer->ByteLength();
    }
    if (!store || store->IsShared() || store->IsResizableByUserJavaScript()) {
        return nullptr;
    }
    return store;
}

template <TypedElement T, class Buffer>
[[nodiscard]] auto newTypedArray(const v8::Local<Buffer> buffer, const std::size_t byteOffset,
                                 const std::size_t length) -> v8::Local<v8::TypedArray> {
    constexpr auto kind = typedKindOf<T>();
    if constexpr (kind == TypedKind::Int8) {
        return v8::Int8Array::New(buffer, byteOffset, length);
    } else if constexpr (kind == TypedKind::Int16) {
        return v8::Int16Array::New(buffer, byteOffset, length);
    } else if constexpr (kind == TypedKind::Uint16) {
        return v8::Uint16Array::New(buffer, byteOffset, length);
    } else if constexpr (kind == TypedKind::Int32) {
        return v8::Int32Array::New(buffer, byteOffset, length);
    } else if constexpr (kind == TypedKind::Uint32) {
        return v8::Uint32Array::New(buffer, byteOffset, length);
    } else if constexpr (kind == TypedKind::BigInt64) {
        return v8::BigInt64Array::New(buffer, byteOffset, length);
    } else if constexpr (kind == TypedKind::BigUint64) {
        return v8::BigUint64Array::New(buffer, byteOffset, length);
    } else if constexpr (kind == TypedKind::Float32) {
        return v8::Float32Array::New(buffer, byteOffset, length);
    } else {
        return v8::Float64Array::New(buffer, byteOffset, length);
    }
}

template <TypedElement T>
[[nodiscard]] auto typedArrayMemory(const v8::Local<v8::Value> value, std::size_t& offset,
                                    std::size_t& length) -> std::shared_ptr<v8::BackingStore> {
    if (typedKindOfValue(value) != typedKindOf<T>()) {
        return nullptr;
    }
    const auto array = value.As<v8::TypedArray>();
    auto store = array->Buffer()->GetBackingStore();
    if (!store || store->IsShared() || store->IsResizableByUserJavaScript()) {
        return nullptr;
    }
    offset = array->ByteOffset();
    length = array->Length();
    return store;
}

} // namespace Alka::Detail
