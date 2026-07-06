#pragma once

#include <Alka/Converter.hpp>
#include <Alka/Error.hpp>
#include <Alka/Invoke.hpp>

#include <v8.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Alka {

namespace Detail {

struct ValueState {
    v8::Isolate* mIsolate = nullptr;
    v8::Global<v8::Value> mValue;
    v8::Global<v8::Context> mContext;
};

[[nodiscard]] inline auto tryCatchMessage(v8::Isolate* isolate,
                                          const v8::Local<v8::Context> context,
                                          const v8::TryCatch& tryCatch) -> std::string {
    std::string message = "JS exception";
    if (tryCatch.HasCaught()) {
        if (const v8::String::Utf8Value text(isolate, tryCatch.Exception()); *text != nullptr) {
            message.assign(*text, static_cast<std::size_t>(text.length()));
        }
        v8::Local<v8::Value> trace;
        if (tryCatch.StackTrace(context).ToLocal(&trace) && trace->IsString()) {
            if (const v8::String::Utf8Value traceText(isolate, trace);
                *traceText != nullptr &&
                std::string_view(*traceText, static_cast<std::size_t>(traceText.length())) !=
                    message) {
                message.append("\n");
                message.append(*traceText, static_cast<std::size_t>(traceText.length()));
            }
        }
    }
    return message;
}

} // namespace Detail

class Value {
public:
    Value() = default;
    Value(const ConvertCtx& cx, const v8::Local<v8::Value> value)
        : mState(std::make_shared<Detail::ValueState>()) {
        mState->mIsolate = cx.mIsolate;
        mState->mValue.Reset(cx.mIsolate, value);
        mState->mContext.Reset(cx.mIsolate, cx.mContext);
    }

    [[nodiscard]] bool valid() const noexcept { return mState && !mState->mValue.IsEmpty(); }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] auto isolate() const noexcept -> v8::Isolate* {
        return mState ? mState->mIsolate : nullptr;
    }
    [[nodiscard]] auto local(v8::Isolate* isolate) const -> v8::Local<v8::Value> {
        return mState->mValue.Get(isolate);
    }

    template <FromJsConvertible T> [[nodiscard]] auto to() const -> Result<T> {
        if (!valid()) {
            return std::unexpected(ConvertError{"empty Alka::Value"});
        }
        const auto isolate = mState->mIsolate;
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        const auto context = mState->mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);
        ConvertCtx cx{isolate, context};
        return fromJs<T>(cx, mState->mValue.Get(isolate));
    }

protected:
    std::shared_ptr<Detail::ValueState> mState;
};

class Object : public Value {
public:
    Object() = default;
    Object(const ConvertCtx& cx, const v8::Local<v8::Object> object) : Value(cx, object) {}

    [[nodiscard]] auto get(const std::string_view key) const -> Result<Value> {
        if (!valid()) {
            return std::unexpected(ConvertError{"empty Alka::Object"});
        }
        const auto isolate = mState->mIsolate;
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        const auto context = mState->mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);
        const ConvertCtx cx{isolate, context};
        const v8::TryCatch tryCatch(isolate);
        v8::Local<v8::Value> out;
        if (!mState->mValue.Get(isolate)
                 .As<v8::Object>()
                 ->Get(context, Detail::v8Str(isolate, key))
                 .ToLocal(&out)) {
            return std::unexpected(
                ConvertError{Detail::tryCatchMessage(isolate, context, tryCatch)});
        }
        return Value(cx, out);
    }

    template <FromJsConvertible T>
    [[nodiscard]] auto getAs(const std::string_view key) const -> Result<T> {
        auto value = get(key);
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        auto out = value->to<T>();
        if (!out) {
            return std::unexpected(ConvertError::forKey(std::move(out).error(), key));
        }
        return out;
    }

    template <class T>
        requires ToJsConvertible<T>
    bool set(const std::string_view key, const T& value) {
        if (!valid()) {
            return false;
        }
        const auto isolate = mState->mIsolate;
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        const auto context = mState->mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);
        ConvertCtx cx{isolate, context};
        return mState->mValue.Get(isolate)
            .As<v8::Object>()
            ->Set(context, Detail::v8Str(isolate, key), toJs(cx, value))
            .FromMaybe(false);
    }
};

template <class Sig> class Function;

template <class R, class... Args> class Function<R(Args...)> : public Value {
public:
    Function() = default;
    Function(const ConvertCtx& cx, const v8::Local<v8::Function> fn) : Value(cx, fn) {}

    [[nodiscard]] auto tryCall(Args... args) const -> Result<R> {
        static_assert((ToJsConvertible<Args> && ...),
                      "Alka: a callback argument type has no Converter<T>::toJs");
        if (!valid()) {
            return std::unexpected(ConvertError{"empty Alka::Function"});
        }
        const auto isolate = mState->mIsolate;
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        const auto context = mState->mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);
        ConvertCtx cx{isolate, context};

        std::array<v8::Local<v8::Value>, sizeof...(Args)> argv{toJs(cx, args)...};
        const v8::TryCatch tryCatch(isolate);
        v8::Local<v8::Value> ret;
        if (!mState->mValue.Get(isolate)
                 .template As<v8::Function>()
                 ->Call(context, v8::Undefined(isolate), static_cast<int>(argv.size()), argv.data())
                 .ToLocal(&ret)) {
            return std::unexpected(
                ConvertError{Detail::tryCatchMessage(isolate, context, tryCatch)});
        }
        if constexpr (std::is_void_v<R>) {
            return {};
        } else {
            static_assert(FromJsConvertible<R>,
                          "Alka: the callback return type has no Converter<T>::fromJs");
            auto out = fromJs<R>(cx, ret);
            if (!out) {
                return std::unexpected(ConvertError{"JS callback returned an incompatible "
                                                    "value: " +
                                                    std::move(out).error().message()});
            }
            return out;
        }
    }

    auto call(Args... args) const -> R {
        auto result = tryCall(std::move(args)...);
        if (!result) {
            throw JsException(std::move(result).error().message());
        }
        if constexpr (!std::is_void_v<R>) {
            return std::move(*result);
        }
    }
};

template <> struct Converter<Value> {
    static auto fromJs(const ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<Value> {
        return Value(cx, value);
    }
    static v8::Local<v8::Value> toJs(const ConvertCtx& cx, const Value& value) {
        if (!value.valid()) {
            return v8::Undefined(cx.mIsolate);
        }
        return value.local(cx.mIsolate);
    }
};

template <> struct Converter<Object> {
    static auto fromJs(const ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<Object> {
        if (value.IsEmpty() || !value->IsObject()) {
            return std::unexpected(ConvertError::typeMismatch("object", describeJsValue(value)));
        }
        return Object(cx, value.As<v8::Object>());
    }
    static auto toJs(const ConvertCtx& cx, const Object& value) -> v8::Local<v8::Value> {
        if (!value.valid()) {
            return v8::Null(cx.mIsolate);
        }
        return value.local(cx.mIsolate);
    }
};

template <class R, class... Args> struct Converter<Function<R(Args...)>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<Function<R(Args...)>> {
        if (value.IsEmpty() || !value->IsFunction()) {
            return std::unexpected(ConvertError::typeMismatch("function", describeJsValue(value)));
        }
        return Function<R(Args...)>(cx, value.As<v8::Function>());
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const Function<R(Args...)>& value) {
        if (!value.valid()) {
            return v8::Undefined(cx.mIsolate);
        }
        return value.local(cx.mIsolate);
    }
};

} // namespace Alka
