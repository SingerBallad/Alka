#pragma once

#include <Alka/Convert/Functional.hpp>
#include <Alka/Core/Error.hpp>
#include <Alka/Js/Value.hpp>

#include <v8.h>

#include <memory>
#include <stop_token>
#include <string>
#include <utility>

// A JS AbortSignal as a C++ stop_token:
//
//   Alka::Task<std::string> fetchText(std::string url, Alka::AbortSignal signal) {
//       std::stop_callback cancel(signal.token(), [&] { request.cancel(); });
//       co_await Alka::offload([&] { request.run(); });
//       signal.throwIfAborted();          // the promise rejects with the JS reason
//       co_return request.text();
//   }
//   // JS: const c = new AbortController(); fetchText(url, c.signal); c.abort();
//
// Any object with `aborted` and addEventListener('abort', ...) qualifies (the
// standard AbortSignal of browsers, Node, and Alka's Engine).

namespace Alka {

class AbortSignal {
public:
    AbortSignal() : mState(std::make_shared<State>()) {}

    [[nodiscard]] auto token() const noexcept -> std::stop_token {
        return mState->mSource.get_token();
    }

    [[nodiscard]] bool aborted() const noexcept { return mState->mSource.stop_requested(); }

    void throwIfAborted() const {
        if (!aborted()) {
            return;
        }
        if (mState->mReason.valid()) {
            throw JsException(ConvertError{"The operation was aborted", mState->mReason.state()});
        }
        throw JsException("The operation was aborted");
    }

    void abortFromJs(Value reason) const {
        mState->mReason = std::move(reason);
        mState->mSource.request_stop();
    }

    struct State {
        std::stop_source mSource;
        Value mReason;
    };
    [[nodiscard]] auto state() const noexcept -> const std::shared_ptr<State>& { return mState; }

private:
    std::shared_ptr<State> mState;
};

template <> struct Converter<AbortSignal> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<AbortSignal> {
        if (value.IsEmpty() || !value->IsObject()) {
            return std::unexpected(
                ConvertError::typeMismatch("AbortSignal", describeJsValue(value)));
        }
        const auto signal = value.As<v8::Object>();
        v8::TryCatch tryCatch(cx.mIsolate);
        AbortSignal out;
        v8::Local<v8::Value> aborted;
        if (!signal->Get(cx.mContext, Detail::cachedKey<"aborted">(cx.mIsolate))
                 .ToLocal(&aborted)) {
            return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
        }
        if (aborted->BooleanValue(cx.mIsolate)) {
            v8::Local<v8::Value> reason;
            if (!signal->Get(cx.mContext, Detail::cachedKey<"reason">(cx.mIsolate))
                     .ToLocal(&reason)) {
                return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
            }
            out.abortFromJs(reason->IsUndefined() ? Value() : Value(cx, reason));
            return out;
        }
        v8::Local<v8::Value> addEventListener;
        if (!signal->Get(cx.mContext, Detail::cachedKey<"addEventListener">(cx.mIsolate))
                 .ToLocal(&addEventListener)) {
            return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
        }
        if (!addEventListener->IsFunction()) {
            return std::unexpected(
                ConvertError::typeMismatch("AbortSignal", "an object without addEventListener"));
        }
        const auto listener = toJs(
            cx, callable(
                    [weak = std::weak_ptr(out.state())](const CallInfo& call) {
                        const auto state = weak.lock();
                        if (!state) {
                            return;
                        }
                        ConvertCtx inner{call.mIsolate, call.mContext};
                        v8::Local<v8::Value> reason;
                        if (call.mThis->IsObject() &&
                            call.mThis.As<v8::Object>()
                                ->Get(call.mContext, Detail::cachedKey<"reason">(call.mIsolate))
                                .ToLocal(&reason) &&
                            !reason->IsUndefined()) {
                            state->mReason = Value(inner, reason);
                        }
                        state->mSource.request_stop();
                    },
                    "abort listener"));
        const auto options = v8::Object::New(cx.mIsolate);
        std::ignore = options->CreateDataProperty(
            cx.mContext, Detail::cachedKey<"once">(cx.mIsolate), v8::True(cx.mIsolate));
        v8::Local<v8::Value> argv[] = {Detail::cachedKey<"abort">(cx.mIsolate), listener, options};
        if (addEventListener.As<v8::Function>()->Call(cx.mContext, signal, 3, argv).IsEmpty()) {
            return std::unexpected(Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch));
        }
        return out;
    }
};

} // namespace Alka
