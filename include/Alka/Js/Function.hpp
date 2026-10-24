#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Js/Value.hpp>

#include <v8.h>

#include <array>
#include <type_traits>
#include <utility>

namespace Alka {

template <class Sig> class Function;

template <class R, class... Args> class Function<R(Args...)> : public Value {
public:
    Function() = default;
    Function(ConvertCtx& cx, const v8::Local<v8::Function> fn) : Value(cx, fn) {}

    [[nodiscard]] auto tryCall(Args... args) const -> Result<R> {
        return invoke(nullptr, std::move(args)...);
    }

    [[nodiscard]] auto tryCallOn(const Value& receiver, Args... args) const -> Result<R> {
        return invoke(&receiver, std::move(args)...);
    }

    auto call(Args... args) const -> R {
        auto result = tryCall(std::move(args)...);
        if (!result) {
            throw JsException(std::move(result).error());
        }
        if constexpr (!std::is_void_v<R>) {
            return std::move(*result);
        }
    }

private:
    auto invoke(const Value* receiver, Args... args) const -> Result<R> {
        static_assert((ToJsConvertible<Args> && ...),
                      "Alka: a callback argument type has no Converter<T>::toJs");
        return enter<R>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<R> {
            std::array<v8::Local<v8::Value>, sizeof...(Args)> argv{toJs(cx, args)...};
            v8::Local<v8::Value> thisArg = v8::Undefined(cx.mIsolate);
            if (receiver != nullptr && receiver->valid()) {
                if (!receiver->belongsTo(cx.mIsolate)) {
                    return std::unexpected(ConvertError{"the receiver belongs to another isolate"});
                }
                thisArg = receiver->local(cx.mIsolate);
            }
            v8::TryCatch tryCatch(cx.mIsolate);
            v8::Local<v8::Value> ret;
            if (!self.template As<v8::Function>()
                     ->Call(cx.mContext, thisArg, static_cast<int>(argv.size()), argv.data())
                     .ToLocal(&ret)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            if constexpr (std::is_void_v<R>) {
                return {};
            } else {
                static_assert(FromJsConvertible<R>,
                              "Alka: the callback return type has no Converter<T>::fromJs");
                auto out = fromJs<R>(cx, ret);
                if (!out) {
                    auto& error = out.error();
                    return std::unexpected(ConvertError{
                        "JS callback returned an incompatible value: " + error.message(),
                        std::move(error.mJsException)});
                }
                return out;
            }
        });
    }
};

template <class R, class... Args> struct Converter<Function<R(Args...)>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<Function<R(Args...)>> {
        if (value.IsEmpty() || !value->IsFunction()) {
            return std::unexpected(ConvertError::typeMismatch("function", describeJsValue(value)));
        }
        return Function<R(Args...)>(cx, value.As<v8::Function>());
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const Function<R(Args...)>& value) {
        return Detail::handleToJs(cx, value, v8::Undefined(cx.mIsolate));
    }
};

} // namespace Alka
