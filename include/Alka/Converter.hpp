#pragma once

#include <Alka/Error.hpp>

#include <v8.h>

#include <concepts>
#include <string_view>
#include <type_traits>

namespace Alka {

struct ConvertCtx {
    v8::Isolate* mIsolate = nullptr;
    v8::Local<v8::Context> mContext;
};

// Primary conversion customization point. Add support for your own type by
// specializing:
//   template<> struct Alka::Converter<MyType> {
//       static Alka::Result<MyType> fromJs(Alka::ConvertCtx&, v8::Local<v8::Value>);
//       static v8::Local<v8::Value>  toJs(Alka::ConvertCtx&, const MyType&);
//   };
// One-directional specializations are fine; the concepts below pick up
// whichever side exists.
template<class T>
struct Converter;

namespace Detail {

template<class T>
using DecayedFor = std::decay_t<const T>;

}  // namespace Detail

template<class T>
concept FromJsConvertible = requires(ConvertCtx& cx, v8::Local<v8::Value> value) {
    {
        Converter<std::remove_cvref_t<T>>::fromJs(cx, value)
    } -> std::same_as<Result<std::remove_cvref_t<T>>>;
};

template<class T>
concept ToJsConvertible = requires(ConvertCtx& cx, const T& value) {
    { Converter<Detail::DecayedFor<T>>::toJs(cx, value) } -> std::same_as<v8::Local<v8::Value>>;
};

template<class T>
concept JsConvertible = FromJsConvertible<T> && ToJsConvertible<T>;

template<FromJsConvertible T>
[[nodiscard]] auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value)
    -> Result<std::remove_cvref_t<T>> {
    return Converter<std::remove_cvref_t<T>>::fromJs(cx, value);
}

template<class T>
    requires ToJsConvertible<T>
[[nodiscard]] auto toJs(ConvertCtx& cx, const T& value) -> v8::Local<v8::Value> {
    return Converter<Detail::DecayedFor<T>>::toJs(cx, value);
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
    if (value->IsObject()) {
        return "object";
    }
    return "value";
}

}  // namespace Alka
