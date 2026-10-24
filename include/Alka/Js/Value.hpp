#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Convert/Primitives.hpp>
#include <Alka/Core/Error.hpp>
#include <Alka/Detail/IsolateLink.hpp>
#include <Alka/Detail/State.hpp>

#include <v8.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Alka {

namespace Detail {

struct ValueState {
    std::shared_ptr<IsolateLink> mLink;
    v8::Global<v8::Value> mValue;
    v8::Global<v8::Context> mContext;
    std::atomic<bool> mDead{false}; // the isolate was disposed

    ValueState() = default;
    ValueState(const ValueState&) = delete;
    ValueState& operator=(const ValueState&) = delete;
    ~ValueState();

    [[nodiscard]] static auto make(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                   v8::Local<v8::Value> value, bool weak = false)
        -> std::shared_ptr<ValueState>;
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

enum class JsType : std::uint8_t {
    Invalid,
    Undefined,
    Null,
    Boolean,
    Number,
    BigInt,
    String,
    Symbol,
    Function,
    Array,
    Promise,
    Object, // any other object
};

class Object;

class Value {
public:
    static constexpr bool alkaIsJsHandle = true; // (a strong reference into JS)

    Value() = default;
    Value(ConvertCtx& cx, const v8::Local<v8::Value> value)
        : mState(Detail::ValueState::make(cx.mIsolate, cx.mContext, value)) {}

    [[nodiscard]] bool valid() const noexcept {
        return mState && !mState->mDead && !mState->mValue.IsEmpty();
    }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] auto isolate() const noexcept -> v8::Isolate* {
        return mState ? mState->mLink->isolate() : nullptr;
    }

    [[nodiscard]] bool belongsTo(const v8::Isolate* isolate) const noexcept {
        return mState && mState->mLink->isolate() == isolate;
    }

    [[nodiscard]] auto local(v8::Isolate* isolate) const -> v8::Local<v8::Value> {
        return valid() && belongsTo(isolate) ? mState->mValue.Get(isolate) : v8::Local<v8::Value>();
    }

    [[nodiscard]] auto creationContext(v8::Isolate* isolate) const -> v8::Local<v8::Context> {
        return valid() && belongsTo(isolate) ? mState->mContext.Get(isolate)
                                             : v8::Local<v8::Context>();
    }

    [[nodiscard]] auto state() const noexcept -> const std::shared_ptr<Detail::ValueState>& {
        return mState;
    }

    template <FromJsConvertible T> [[nodiscard]] auto to() const -> Result<T> {
        return enter<T>(
            [](ConvertCtx& cx, const v8::Local<v8::Value> value) { return fromJs<T>(cx, value); });
    }

    [[nodiscard]] auto type() const -> JsType {
        return enter<JsType>([](ConvertCtx&, const v8::Local<v8::Value> v) -> Result<JsType> {
                   if (v->IsUndefined()) {
                       return JsType::Undefined;
                   }
                   if (v->IsNull()) {
                       return JsType::Null;
                   }
                   if (v->IsBoolean()) {
                       return JsType::Boolean;
                   }
                   if (v->IsNumber()) {
                       return JsType::Number;
                   }
                   if (v->IsBigInt()) {
                       return JsType::BigInt;
                   }
                   if (v->IsString()) {
                       return JsType::String;
                   }
                   if (v->IsSymbol()) {
                       return JsType::Symbol;
                   }
                   if (v->IsFunction()) {
                       return JsType::Function;
                   }
                   if (v->IsArray()) {
                       return JsType::Array;
                   }
                   if (v->IsPromise()) {
                       return JsType::Promise;
                   }
                   return JsType::Object;
               })
            .value_or(JsType::Invalid);
    }
    [[nodiscard]] bool isUndefined() const { return type() == JsType::Undefined; }
    [[nodiscard]] bool isNull() const { return type() == JsType::Null; }
    [[nodiscard]] bool isNullish() const {
        const auto t = type();
        return t == JsType::Undefined || t == JsType::Null;
    }
    [[nodiscard]] bool isBoolean() const { return type() == JsType::Boolean; }
    [[nodiscard]] bool isNumber() const { return type() == JsType::Number; }
    [[nodiscard]] bool isBigInt() const { return type() == JsType::BigInt; }
    [[nodiscard]] bool isString() const { return type() == JsType::String; }
    [[nodiscard]] bool isSymbol() const { return type() == JsType::Symbol; }
    [[nodiscard]] bool isFunction() const { return type() == JsType::Function; }
    [[nodiscard]] bool isArray() const { return type() == JsType::Array; }
    [[nodiscard]] bool isPromise() const { return type() == JsType::Promise; }
    [[nodiscard]] bool isObject() const {
        const auto t = type();
        return t == JsType::Function || t == JsType::Array || t == JsType::Promise ||
               t == JsType::Object;
    }

    [[nodiscard]] auto typeOf() const -> std::string_view {
        switch (type()) {
        case JsType::Invalid:
            return "";
        case JsType::Undefined:
            return "undefined";
        case JsType::Null:
            return "object";
        case JsType::Boolean:
            return "boolean";
        case JsType::Number:
            return "number";
        case JsType::BigInt:
            return "bigint";
        case JsType::String:
            return "string";
        case JsType::Symbol:
            return "symbol";
        case JsType::Function:
            return "function";
        case JsType::Array:
        case JsType::Promise:
        case JsType::Object:
            return "object";
        }
        return "";
    }

    [[nodiscard]] bool strictEquals(const Value& other) const {
        if (!other.valid() || !belongsTo(other.isolate())) {
            return false;
        }
        return enter<bool>([&](ConvertCtx& cx, const v8::Local<v8::Value> v) -> Result<bool> {
                   return v->StrictEquals(other.local(cx.mIsolate));
               })
            .value_or(false);
    }

    [[nodiscard]] auto toString() const -> Result<std::string> {
        return enter<std::string>(
            [](ConvertCtx& cx, const v8::Local<v8::Value> v) -> Result<std::string> {
                if (v->IsSymbol()) {
                    const auto description = v.As<v8::Symbol>()->Description(cx.mIsolate);
                    return "Symbol(" +
                           (description->IsString()
                                ? Detail::stringToUtf8(cx.mIsolate, description.As<v8::String>())
                                : std::string()) +
                           ")";
                }
                v8::TryCatch tryCatch(cx.mIsolate);
                v8::Local<v8::String> text;
                if (!v->ToString(cx.mContext).ToLocal(&text)) {
                    return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
                }
                return Detail::stringToUtf8(cx.mIsolate, text);
            });
    }

    [[nodiscard]] auto toJson(const int indent = 0) const -> Result<std::string> {
        return enter<std::string>([&](ConvertCtx& cx,
                                      const v8::Local<v8::Value> v) -> Result<std::string> {
            v8::TryCatch tryCatch(cx.mIsolate);
            v8::Local<v8::String> text;
            const auto gap =
                indent > 0
                    ? Detail::v8Str(cx.mIsolate, std::string(static_cast<std::size_t>(indent), ' '))
                    : v8::Local<v8::String>();
            if (!v8::JSON::Stringify(cx.mContext, v, gap).ToLocal(&text)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            if (v->IsUndefined() || v->IsFunction() || v->IsSymbol()) {
                return std::unexpected(ConvertError{"this value has no JSON form"});
            }
            return Detail::stringToUtf8(cx.mIsolate, text);
        });
    }

    template <class R = Value, class... Args>
    [[nodiscard]] auto tryCall(const Args&... args) const -> Result<R> {
        return callOn<R>(nullptr, args...);
    }

    template <class R = Value, class... Args>
    [[nodiscard]] auto tryCallOn(const Value& receiver, const Args&... args) const -> Result<R> {
        return callOn<R>(&receiver, args...);
    }

    template <class... Args>
    [[nodiscard]] auto tryConstruct(const Args&... args) const -> Result<Object>;

protected:
    friend class Object;
    friend auto exceptionValue(const ConvertError& error) -> Value;
    friend auto exceptionValue(const JsException& error) -> Value;
    template <class T> friend class WeakValue;
    template <class T> friend class Retained;

    explicit Value(std::shared_ptr<Detail::ValueState> state) : mState(std::move(state)) {}

    template <class R, class Fn> auto enter(Fn&& fn) const -> Result<R> {
        if (!mState) {
            return std::unexpected(ConvertError{"empty Alka handle"});
        }
        if (mState->mDead) {
            return std::unexpected(ConvertError{"the handle's isolate was disposed"});
        }
        const auto isolate = mState->mLink->isolate();
        const bool entered = v8::Isolate::TryGetCurrent() == isolate;
        if (!entered && !mState->mLink->onIsolateThread()) {
            return std::unexpected(ConvertError{"Alka handle used off its isolate's thread"});
        }
        if (mState->mValue.IsEmpty()) {
            return std::unexpected(ConvertError{"the JS value was garbage collected"});
        }
        std::optional<v8::Isolate::Scope> isolateScope;
        if (!entered) {
            isolateScope.emplace(isolate);
        }
        const v8::HandleScope handleScope(isolate);
        const auto context = mState->mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);
        ConvertCtx cx{isolate, context};
        try {
            return std::forward<Fn>(fn)(cx, mState->mValue.Get(isolate));
        } catch (const JsException& e) {
            return std::unexpected(ConvertError{e.message(), e.jsException()});
        }
    }

    template <class R, class... Args>
    [[nodiscard]] auto callOn(const Value* receiver, const Args&... args) const -> Result<R> {
        static_assert((ToJsConvertible<Args> && ...),
                      "Alka: an argument type has no Converter<T>::toJs");
        return enter<R>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<R> {
            if (!self->IsFunction()) {
                return std::unexpected(
                    ConvertError::typeMismatch("function", describeJsValue(self)));
            }
            v8::Local<v8::Value> thisArg = v8::Undefined(cx.mIsolate);
            if (receiver != nullptr && receiver->valid()) {
                if (!receiver->belongsTo(cx.mIsolate)) {
                    return std::unexpected(ConvertError{"the receiver belongs to another isolate"});
                }
                thisArg = receiver->local(cx.mIsolate);
            }
            std::array<v8::Local<v8::Value>, sizeof...(Args)> argv{toJs(cx, args)...};
            v8::TryCatch tryCatch(cx.mIsolate);
            v8::Local<v8::Value> result;
            if (!self.As<v8::Function>()
                     ->Call(cx.mContext, thisArg, static_cast<int>(argv.size()), argv.data())
                     .ToLocal(&result)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            if constexpr (std::is_void_v<R>) {
                return {};
            } else {
                return fromJs<R>(cx, result);
            }
        });
    }

    std::shared_ptr<Detail::ValueState> mState;
};

[[nodiscard]] inline auto exceptionValue(const ConvertError& error) -> Value {
    return Value(error.mJsException);
}
[[nodiscard]] inline auto exceptionValue(const JsException& error) -> Value {
    return Value(error.jsException());
}

class Object : public Value {
public:
    Object() = default;
    Object(ConvertCtx& cx, const v8::Local<v8::Object> object) : Value(cx, object) {}

    [[nodiscard]] static auto create(ConvertCtx& cx) -> Object {
        return {cx, v8::Object::New(cx.mIsolate)};
    }

    [[nodiscard]] static auto global(ConvertCtx& cx) -> Object {
        return {cx, cx.mContext->Global()};
    }

    [[nodiscard]] auto has(const std::string_view key) const -> Result<bool> {
        return enter<bool>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<bool> {
            v8::TryCatch tryCatch(cx.mIsolate);
            const auto found =
                self.As<v8::Object>()->Has(cx.mContext, Detail::v8Str(cx.mIsolate, key));
            if (found.IsNothing()) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            return found.FromJust();
        });
    }

    [[nodiscard]] auto hasOwn(const std::string_view key) const -> Result<bool> {
        return enter<bool>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<bool> {
            v8::TryCatch tryCatch(cx.mIsolate);
            const auto found =
                self.As<v8::Object>()->HasOwnProperty(cx.mContext, Detail::v8Str(cx.mIsolate, key));
            if (found.IsNothing()) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            return found.FromJust();
        });
    }

    [[nodiscard]] auto remove(const std::string_view key) const -> Result<bool> {
        return enter<bool>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<bool> {
            v8::TryCatch tryCatch(cx.mIsolate);
            const auto removed =
                self.As<v8::Object>()->Delete(cx.mContext, Detail::v8Str(cx.mIsolate, key));
            if (removed.IsNothing()) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            return removed.FromJust();
        });
    }

    [[nodiscard]] auto keys() const -> Result<std::vector<std::string>> {
        return enter<std::vector<std::string>>([](ConvertCtx& cx, const v8::Local<v8::Value> self)
                                                   -> Result<std::vector<std::string>> {
            v8::TryCatch tryCatch(cx.mIsolate);
            v8::Local<v8::Array> names;
            if (!self.As<v8::Object>()
                     ->GetOwnPropertyNames(
                         cx.mContext,
                         static_cast<v8::PropertyFilter>(v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS),
                         v8::KeyConversionMode::kConvertToString)
                     .ToLocal(&names)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            std::vector<std::string> out;
            out.reserve(names->Length());
            for (std::uint32_t i = 0; i < names->Length(); ++i) {
                v8::Local<v8::Value> name;
                if (!names->Get(cx.mContext, i).ToLocal(&name) || !name->IsString()) {
                    return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
                }
                out.push_back(Detail::stringToUtf8(cx.mIsolate, name.As<v8::String>()));
            }
            return out;
        });
    }

    template <class R = Value, class... Args>
    [[nodiscard]] auto tryCallMethod(const std::string_view name, const Args&... args) const
        -> Result<R> {
        auto method = get(name);
        if (!method) {
            return std::unexpected(std::move(method).error());
        }
        if (!method->isFunction()) {
            return std::unexpected(ConvertError{"`" + std::string(name) + "` is not a function"});
        }
        return method->callOn<R>(this, args...);
    }

    [[nodiscard]] auto get(const std::string_view key) const -> Result<Value> {
        return enter<Value>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<Value> {
            v8::TryCatch tryCatch(cx.mIsolate);
            v8::Local<v8::Value> out;
            if (!self.As<v8::Object>()
                     ->Get(cx.mContext, Detail::v8Str(cx.mIsolate, key))
                     .ToLocal(&out)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            return Value(cx, out);
        });
    }

    template <FromJsConvertible T>
    [[nodiscard]] auto getAs(const std::string_view key) const -> Result<T> {
        return enter<T>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<T> {
            v8::TryCatch tryCatch(cx.mIsolate);
            v8::Local<v8::Value> out;
            if (!self.As<v8::Object>()
                     ->Get(cx.mContext, Detail::v8Str(cx.mIsolate, key))
                     .ToLocal(&out)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            auto converted = fromJs<T>(cx, out);
            if (!converted) {
                return std::unexpected(ConvertError::forKey(std::move(converted).error(), key));
            }
            return converted;
        });
    }

    template <class T>
        requires ToJsConvertible<T>
    auto set(const std::string_view key, const T& value) const -> Result<void> {
        return enter<void>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<void> {
            v8::TryCatch tryCatch(cx.mIsolate);
            if (!self.As<v8::Object>()
                     ->Set(cx.mContext, Detail::v8Str(cx.mIsolate, key), toJs(cx, value))
                     .FromMaybe(false)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            return {};
        });
    }
};

template <class... Args> auto Value::tryConstruct(const Args&... args) const -> Result<Object> {
    static_assert((ToJsConvertible<Args> && ...),
                  "Alka: an argument type has no Converter<T>::toJs");
    return enter<Object>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<Object> {
        if (!self->IsFunction()) {
            return std::unexpected(
                ConvertError::typeMismatch("constructor", describeJsValue(self)));
        }
        std::array<v8::Local<v8::Value>, sizeof...(Args)> argv{toJs(cx, args)...};
        v8::TryCatch tryCatch(cx.mIsolate);
        v8::Local<v8::Object> made;
        if (!self.As<v8::Function>()
                 ->NewInstance(cx.mContext, static_cast<int>(argv.size()), argv.data())
                 .ToLocal(&made)) {
            return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
        }
        return Object(cx, made);
    });
}

// A JS array.
class Array : public Object {
public:
    Array() = default;
    Array(ConvertCtx& cx, const v8::Local<v8::Array> array) : Object(cx, array) {}

    [[nodiscard]] static auto create(ConvertCtx& cx, const std::size_t length = 0) -> Array {
        return Array(cx, v8::Array::New(cx.mIsolate, static_cast<int>(length)));
    }

    [[nodiscard]] auto size() const -> Result<std::size_t> {
        return enter<std::size_t>(
            [](ConvertCtx&, const v8::Local<v8::Value> self) -> Result<std::size_t> {
                return self.As<v8::Array>()->Length();
            });
    }

    [[nodiscard]] auto get(const std::size_t index) const -> Result<Value> {
        return enter<Value>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<Value> {
            v8::TryCatch tryCatch(cx.mIsolate);
            v8::Local<v8::Value> out;
            if (!self.As<v8::Array>()
                     ->Get(cx.mContext, static_cast<std::uint32_t>(index))
                     .ToLocal(&out)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            return Value(cx, out);
        });
    }
    using Object::get;

    template <FromJsConvertible T>
    [[nodiscard]] auto getAs(const std::size_t index) const -> Result<T> {
        return enter<T>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<T> {
            v8::TryCatch tryCatch(cx.mIsolate);
            v8::Local<v8::Value> out;
            if (!self.As<v8::Array>()
                     ->Get(cx.mContext, static_cast<std::uint32_t>(index))
                     .ToLocal(&out)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            auto converted = fromJs<T>(cx, out);
            if (!converted) {
                return std::unexpected(
                    ConvertError::forElement(std::move(converted).error(), index));
            }
            return converted;
        });
    }
    using Object::getAs;

    template <class T>
        requires ToJsConvertible<T>
    auto set(const std::size_t index, const T& value) const -> Result<void> {
        return enter<void>([&](ConvertCtx& cx, const v8::Local<v8::Value> self) -> Result<void> {
            v8::TryCatch tryCatch(cx.mIsolate);
            if (!self.As<v8::Array>()
                     ->Set(cx.mContext, static_cast<std::uint32_t>(index), toJs(cx, value))
                     .FromMaybe(false)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            return {};
        });
    }
    using Object::set;

    template <class T>
        requires ToJsConvertible<T>
    auto push(const T& value) const -> Result<void> {
        auto length = size();
        if (!length) {
            return std::unexpected(std::move(length).error());
        }
        return set(*length, value);
    }
};

[[nodiscard]] inline auto parseJson(ConvertCtx& cx, const std::string_view text) -> Result<Value> {
    v8::TryCatch tryCatch(cx.mIsolate);
    v8::Local<v8::String> source;
    if (text.size() > static_cast<std::size_t>(v8::String::kMaxLength) ||
        !v8::String::NewFromUtf8(cx.mIsolate, text.data(), v8::NewStringType::kNormal,
                                 static_cast<int>(text.size()))
             .ToLocal(&source)) {
        return std::unexpected(ConvertError{"the JSON text is too long for a JS string"});
    }
    v8::Local<v8::Value> parsed;
    if (!v8::JSON::Parse(cx.mContext, source).ToLocal(&parsed)) {
        return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
    }
    return Value(cx, parsed);
}

template <class T = Value> class WeakValue {
    static_assert(std::is_base_of_v<Value, T>,
                  "Alka: WeakValue<T> takes Value, Object or Function");

public:
    WeakValue() = default;
    // Isolate thread.
    explicit WeakValue(const T& value) {
        if (!value.valid() || !value.mState->mLink->onIsolateThread()) {
            return;
        }
        const auto isolate = value.isolate();
        const v8::HandleScope scope(isolate);
        mState = Detail::ValueState::make(isolate, value.mState->mContext.Get(isolate),
                                          value.local(isolate), true);
    }

    [[nodiscard]] bool expired() const noexcept {
        return !mState || mState->mDead || mState->mValue.IsEmpty() || mState->mContext.IsEmpty();
    }

    [[nodiscard]] auto lock() const -> T {
        if (expired() || !mState->mLink->onIsolateThread()) {
            return T{};
        }
        const auto isolate = mState->mLink->isolate();
        const v8::HandleScope scope(isolate);
        auto strong = Detail::ValueState::make(isolate, mState->mContext.Get(isolate),
                                               mState->mValue.Get(isolate));
        T out;
        static_cast<Value&>(out) = Value(std::move(strong));
        return out;
    }

private:
    std::shared_ptr<Detail::ValueState> mState;
};

namespace Detail {

[[nodiscard]] inline auto handleToJs(ConvertCtx& cx, const Value& value,
                                     const v8::Local<v8::Value> empty) -> v8::Local<v8::Value> {
    if (!value.valid()) {
        return empty;
    }
    if (!value.belongsTo(cx.mIsolate)) {
        throw JsException("an Alka handle belongs to another isolate");
    }
    return value.local(cx.mIsolate);
}

} // namespace Detail

template <> struct Converter<Value> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<Value> {
        return Value(cx, value);
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const Value& value) {
        return Detail::handleToJs(cx, value, v8::Undefined(cx.mIsolate));
    }
};

template <> struct Converter<Object> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<Object> {
        if (value.IsEmpty() || !value->IsObject()) {
            return std::unexpected(ConvertError::typeMismatch("object", describeJsValue(value)));
        }
        return Object(cx, value.As<v8::Object>());
    }
    static auto toJs(ConvertCtx& cx, const Object& value) -> v8::Local<v8::Value> {
        return Detail::handleToJs(cx, value, v8::Null(cx.mIsolate));
    }
};

template <> struct Converter<Array> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<Array> {
        if (value.IsEmpty() || !value->IsArray()) {
            return std::unexpected(ConvertError::typeMismatch("array", describeJsValue(value)));
        }
        return Array(cx, value.As<v8::Array>());
    }
    static auto toJs(ConvertCtx& cx, const Array& value) -> v8::Local<v8::Value> {
        return Detail::handleToJs(cx, value, v8::Null(cx.mIsolate));
    }
};

} // namespace Alka
