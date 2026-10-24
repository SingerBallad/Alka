#pragma once

#include <Alka/Binding/Policy.hpp>
#include <Alka/Core/Error.hpp>

#include <v8.h>

#include <concepts>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Alka {

struct ConvertCtx {
    v8::Isolate* mIsolate = nullptr;
    v8::Local<v8::Context> mContext;
    Int64Mode mInt64Mode = kDefaultInt64Mode; // how 64-bit integers go to JS
    v8::Local<v8::Object> mReceiver;          // `this` of the bound member call, if any

    ConvertCtx() = default;
    ConvertCtx(v8::Isolate* isolate, const v8::Local<v8::Context> context,
               const Int64Mode int64Mode = kDefaultInt64Mode,
               const v8::Local<v8::Object> receiver = {})
        : mIsolate(isolate), mContext(context), mInt64Mode(int64Mode), mReceiver(receiver) {}
};

// Primary conversion customization point.
// Add support for your own type by specializing:
//   template<> struct Alka::Converter<MyType> {
//       static Alka::Result<MyType> fromJs(Alka::ConvertCtx&, v8::Local<v8::Value>);
//       static v8::Local<v8::Value>  toJs(Alka::ConvertCtx&, const MyType&);
//   };
// One-directional specializations are fine; the concepts below pick up
// whichever side exists.
template <class T> struct Converter;

namespace Detail {

template <class T> using DecayedFor = std::decay_t<const T>;

} // namespace Detail

template <class T>
concept FromJsConvertible = requires(ConvertCtx& cx, v8::Local<v8::Value> value) {
    {
        Converter<std::remove_cvref_t<T>>::fromJs(cx, value)
    } -> std::same_as<Result<std::remove_cvref_t<T>>>;
};

template <class T>
concept ToJsConvertible = requires(ConvertCtx& cx, const T& value) {
    { Converter<Detail::DecayedFor<T>>::toJs(cx, value) } -> std::same_as<v8::Local<v8::Value>>;
};

template <class T>
concept JsConvertible = FromJsConvertible<T> && ToJsConvertible<T>;

template <FromJsConvertible T>
[[nodiscard]] auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value)
    -> Result<std::remove_cvref_t<T>> {
    return Converter<std::remove_cvref_t<T>>::fromJs(cx, value);
}

template <class T>
    requires ToJsConvertible<T>
[[nodiscard]] auto toJs(ConvertCtx& cx, const T& value) -> v8::Local<v8::Value> {
    return Converter<Detail::DecayedFor<T>>::toJs(cx, value);
}

template <class T>
    requires(!std::is_lvalue_reference_v<T> && !ToJsConvertible<T> &&
             requires(ConvertCtx& cx, T&& value) {
                 {
                     Converter<std::remove_cvref_t<T>>::toJs(cx, std::move(value))
                 } -> std::same_as<v8::Local<v8::Value>>;
             })
[[nodiscard]] auto toJs(ConvertCtx& cx, T&& value) -> v8::Local<v8::Value> {
    return Converter<std::remove_cvref_t<T>>::toJs(cx, std::forward<T>(value));
}

[[nodiscard]] inline auto describeJsValue(const v8::Local<v8::Value> value) -> std::string_view {
    if (value.IsEmpty()) {
        return "nothing";
    }
    if (value->IsUndefined()) {
        return "undefined";
    }
    if (value->IsNull()) {
        return "null";
    }
    if (value->IsBoolean()) {
        return "boolean";
    }
    if (value->IsNumber()) {
        return "number";
    }
    if (value->IsBigInt()) {
        return "bigint";
    }
    if (value->IsString()) {
        return "string";
    }
    if (value->IsSymbol()) {
        return "symbol";
    }
    if (value->IsFunction()) {
        return "function";
    }
    if (value->IsArray()) {
        return "array";
    }
    if (value->IsTypedArray()) {
        if (value->IsFloat64Array()) {
            return "Float64Array";
        }
        if (value->IsFloat32Array()) {
            return "Float32Array";
        }
        if (value->IsInt32Array()) {
            return "Int32Array";
        }
        if (value->IsUint32Array()) {
            return "Uint32Array";
        }
        if (value->IsInt16Array()) {
            return "Int16Array";
        }
        if (value->IsUint16Array()) {
            return "Uint16Array";
        }
        if (value->IsInt8Array()) {
            return "Int8Array";
        }
        if (value->IsUint8Array()) {
            return "Uint8Array";
        }
        if (value->IsUint8ClampedArray()) {
            return "Uint8ClampedArray";
        }
        if (value->IsBigInt64Array()) {
            return "BigInt64Array";
        }
        if (value->IsBigUint64Array()) {
            return "BigUint64Array";
        }
        return "typed array";
    }
    if (value->IsArrayBuffer()) {
        return "ArrayBuffer";
    }
    if (value->IsSharedArrayBuffer()) {
        return "SharedArrayBuffer";
    }
    if (value->IsDataView()) {
        return "DataView";
    }
    if (value->IsPromise()) {
        return "Promise";
    }
    if (value->IsMap()) {
        return "Map";
    }
    if (value->IsSet()) {
        return "Set";
    }
    if (value->IsDate()) {
        return "Date";
    }
    if (value->IsRegExp()) {
        return "RegExp";
    }
    if (value->IsNativeError()) {
        return "Error";
    }
    if (value->IsObject()) {
        return "object";
    }
    return "value";
}

} // namespace Alka
