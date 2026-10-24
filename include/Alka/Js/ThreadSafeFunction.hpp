#pragma once

#include <Alka/Core/Error.hpp>
#include <Alka/Js/Function.hpp>
#include <Alka/Runtime/Dispatcher.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

// A JS function C++ may call from any thread:
//
//   void watch(std::string path, Alka::ThreadSafeFunction<void(std::string)> onLine) {
//       std::thread([path, onLine] {
//           for (auto& line : tail(path)) onLine.post(line);   // any thread
//       }).detach();
//   }

namespace Alka {

template <class Sig> class ThreadSafeFunction;

template <class R, class... Args> class ThreadSafeFunction<R(Args...)> {
public:
    using ErrorHandler = std::function<void(const ConvertError&)>;

    ThreadSafeFunction() = default;

    explicit ThreadSafeFunction(Function<R(Args...)> function, ErrorHandler onError = {})
        : mState(std::make_shared<State>()) {
        mState->mDispatcher = Dispatcher(function.isolate());
        mState->mFunction = std::move(function);
        mState->mOnError = std::move(onError);
    }

    [[nodiscard]] bool valid() const noexcept {
        return mState && mState->mFunction.valid() && mState->mDispatcher.alive();
    }
    explicit operator bool() const noexcept { return valid(); }

    bool post(Args... args) const {
        if (!mState) {
            return false;
        }
        return mState->mDispatcher.post(
            [state = mState, tuple = std::make_tuple(std::move(args)...)]() mutable {
                auto result = std::apply(
                    [&](auto&... a) { return state->mFunction.tryCall(std::move(a)...); }, tuple);
                if (!result && state->mOnError) {
                    state->mOnError(result.error());
                }
            });
    }

    auto call(Args... args) const -> std::future<Result<R>> {
        auto promise = std::make_shared<std::promise<Result<R>>>();
        auto future = promise->get_future();
        const bool queued =
            mState &&
            mState->mDispatcher.post([state = mState, promise,
                                      tuple = std::make_tuple(std::move(args)...)]() mutable {
                promise->set_value(std::apply(
                    [&](auto&... a) { return state->mFunction.tryCall(std::move(a)...); }, tuple));
            });
        if (!queued) {
            promise->set_value(std::unexpected(ConvertError{"the function's isolate is gone"}));
        }
        return future;
    }

    void keepAlive(const bool on) const {
        if (!mState) {
            return;
        }
        if (mState->mRetained.exchange(on) != on) {
            on ? mState->mDispatcher.retain() : mState->mDispatcher.release();
        }
    }

private:
    struct State {
        Function<R(Args...)> mFunction;
        Dispatcher mDispatcher;
        ErrorHandler mOnError;
        std::atomic<bool> mRetained{false};

        State() = default;
        State(const State&) = delete;
        State& operator=(const State&) = delete;
        ~State() {
            if (mRetained.load()) {
                mDispatcher.release();
            }
        }
    };
    std::shared_ptr<State> mState;
};

template <class R, class... Args> struct Converter<ThreadSafeFunction<R(Args...)>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<ThreadSafeFunction<R(Args...)>> {
        auto function = Converter<Function<R(Args...)>>::fromJs(cx, value);
        if (!function) {
            return std::unexpected(std::move(function).error());
        }
        return ThreadSafeFunction<R(Args...)>(std::move(*function));
    }
};

} // namespace Alka
