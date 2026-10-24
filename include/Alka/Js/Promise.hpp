#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Js/Value.hpp>
#include <Alka/Runtime/Dispatcher.hpp>

#include <v8.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Alka {

template <class T> class Settler;

// A JS promise settled from C++.
//
//   Alka::fn<"load", [](std::string path) {
//       auto promise = Alka::Promise<std::string>::create();
//       gJobs.push_back({std::move(path), promise.settler()});
//       return promise;   // -> load(path: string): Promise<string>
//   }>()
template <class T = void> class Promise : public Value {
public:
    Promise() = default;
    explicit Promise(ConvertCtx& cx) {
        v8::Local<v8::Promise::Resolver> resolver;
        if (v8::Promise::Resolver::New(cx.mContext).ToLocal(&resolver)) {
            static_cast<Value&>(*this) = Value(cx, resolver);
        }
    }

    [[nodiscard]] static auto create() -> Promise {
        const auto isolate = v8::Isolate::TryGetCurrent();
        if (isolate == nullptr || !isolate->InContext()) {
            return Promise();
        }
        ConvertCtx cx{isolate, isolate->GetCurrentContext()};
        return Promise(cx);
    }

    template <class U = T>
        requires(!std::is_void_v<U>)
    auto resolve(const U& value) const -> Result<void> {
        static_assert(ToJsConvertible<T>, "Alka: Promise<T> needs a Converter<T>::toJs");
        return settle([&](ConvertCtx& cx, const v8::Local<v8::Promise::Resolver> resolver) {
            return resolver->Resolve(cx.mContext, toJs(cx, static_cast<const T&>(value)));
        });
    }

    template <class U = T>
        requires std::is_void_v<U>
    [[nodiscard]] auto resolve() const -> Result<void> {
        return settle([](ConvertCtx& cx, const v8::Local<v8::Promise::Resolver> resolver) {
            return resolver->Resolve(cx.mContext, v8::Undefined(cx.mIsolate));
        });
    }

    [[nodiscard]] auto reject(const std::string_view message) const -> Result<void> {
        return settle([&](ConvertCtx& cx, const v8::Local<v8::Promise::Resolver> resolver) {
            return resolver->Reject(cx.mContext,
                                    v8::Exception::Error(Detail::v8Str(cx.mIsolate, message)));
        });
    }

    [[nodiscard]] auto reject(const Value& reason) const -> Result<void> {
        return settle([&](ConvertCtx& cx, const v8::Local<v8::Promise::Resolver> resolver) {
            return resolver->Reject(cx.mContext, toJs(cx, reason));
        });
    }

    [[nodiscard]] auto settler() const -> Settler<T>;

    [[nodiscard]] auto promise(v8::Isolate* isolate) const -> v8::Local<v8::Promise> {
        return local(isolate).template As<v8::Promise::Resolver>()->GetPromise();
    }

private:
    template <class Fn> auto settle(Fn&& fn) const -> Result<void> {
        return enter<void>([&](ConvertCtx& cx, const v8::Local<v8::Value> local) -> Result<void> {
            if (!fn(cx, local.As<v8::Promise::Resolver>()).FromMaybe(false)) {
                return std::unexpected(ConvertError{"the promise could not be settled"});
            }
            return {};
        });
    }
};

template <class T = void> class Settler {
public:
    Settler() = default;
    Settler(Promise<T> promise, Dispatcher dispatcher)
        : mState(std::make_shared<State>(std::move(promise), std::move(dispatcher))) {}

    template <class U = T>
        requires(!std::is_void_v<U>)
    bool resolve(U value) const {
        return deliver([value = std::move(value)](const Promise<T>& promise) {
            (void)promise.resolve(value);
        });
    }

    template <class U = T>
        requires std::is_void_v<U>
    [[nodiscard]] bool resolve() const {
        return deliver([](const Promise<T>& promise) { (void)promise.resolve(); });
    }

    [[nodiscard]] bool reject(std::string message) const {
        return deliver([message = std::move(message)](const Promise<T>& promise) {
            (void)promise.reject(message);
        });
    }

    [[nodiscard]] bool settled() const noexcept { return !mState || mState->mSettled.load(); }

private:
    struct State {
        State(Promise<T> promise, Dispatcher dispatcher)
            : mPromise(std::move(promise)), mDispatcher(std::move(dispatcher)) {
            mDispatcher.retain();
        }
        State(const State&) = delete;
        State& operator=(const State&) = delete;
        ~State() {
            if (!mSettled.load()) {
                mDispatcher.release();
            }
        }
        Promise<T> mPromise;
        Dispatcher mDispatcher;
        std::atomic<bool> mSettled{false};
    };

    template <class Fn> bool deliver(Fn&& fn) const {
        if (!mState || mState->mSettled.exchange(true)) {
            return false;
        }
        const bool posted = mState->mDispatcher.post(
            [state = mState, fn = std::forward<Fn>(fn)]() mutable { fn(state->mPromise); });
        mState->mDispatcher.release();
        return posted;
    }

    std::shared_ptr<State> mState;
};

template <class T> auto Promise<T>::settler() const -> Settler<T> {
    return Settler<T>(*this, Dispatcher(isolate()));
}

template <class T> struct Converter<Promise<T>> {
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const Promise<T>& value) {
        if (!value.valid()) {
            return v8::Undefined(cx.mIsolate);
        }
        if (!value.belongsTo(cx.mIsolate)) {
            throw JsException("an Alka handle belongs to another isolate");
        }
        return value.promise(cx.mIsolate);
    }
};

} // namespace Alka
