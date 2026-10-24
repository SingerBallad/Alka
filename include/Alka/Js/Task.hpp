#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Js/Promise.hpp>
#include <Alka/Js/Value.hpp>

#include <v8.h>

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

// C++20 coroutines as JS promises.
//
//   Alka::Task<std::string> fetchTitle(std::string url) {
//       auto page = co_await Alka::offload([url] { return httpGet(url); });  // worker thread
//       auto parsed = co_await Alka::await<Alka::Object>(jsParser.call(page));  // a JS promise
//       co_return parsed.getAs<std::string>("title").value();
//   }
//
// After a non-Alka awaitable resumed the coroutine on another thread,
// `co_await Alka::resumeOn(dispatcher)` brings it back to the isolate thread.
//   Alka::fn<&fetchTitle>()     // -> fetchTitle(url: string): Promise<string>

namespace Alka {

template <class T = void> class Task;

namespace Detail {

template <class T, class Fn> void settleOnIsolate(const Promise<T>& promise, Fn&& fn) {
    if (!promise.valid()) {
        return;
    }
    if (const auto& link = promise.state()->mLink; !link->onIsolateThread()) {
        (void)link->postTask(std::forward<Fn>(fn));
        return;
    }
    std::forward<Fn>(fn)();
}

template <class T> void rejectWithCurrentException(const Promise<T>& promise) {
    if (!promise.valid()) {
        return;
    }
    if (!promise.state()->mLink->onIsolateThread()) {
        settleOnIsolate(promise, [promise, error = std::current_exception()] {
            try {
                std::rethrow_exception(error);
            } catch (...) {
                rejectWithCurrentException(promise);
            }
        });
        return;
    }
    if (const auto isolate = promise.isolate(); isolate != nullptr) {
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope scope(isolate);
        const auto context = promise.creationContext(isolate);
        if (const auto value = context.IsEmpty() ? v8::Local<v8::Value>()
                                                 : registeredErrorValue(isolate, context,
                                                                        std::current_exception());
            !value.IsEmpty()) {
            ConvertCtx cx{isolate, context};
            (void)promise.reject(Value(cx, value));
            return;
        }
    }
    try {
        throw;
    } catch (const JsException& e) {
        if (const auto thrown = exceptionValue(e); !thrown.valid() || !promise.reject(thrown)) {
            (void)promise.reject(e.message()); // (e.g. a value of another isolate)
        }
    } catch (const std::exception& e) {
        (void)promise.reject(e.what());
    } catch (...) {
        (void)promise.reject("unknown C++ exception in a coroutine");
    }
}

template <class T> struct TaskPromiseBase {
    Promise<T> mPromise = Promise<T>::create();

    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void unhandled_exception() { rejectWithCurrentException(mPromise); }
};

template <class T, class... V> void resolveOrReject(const Promise<T>& promise, const V&... value) {
    try {
        if (auto done = promise.resolve(value...); !done) {
            (void)promise.reject(done.error().message());
        }
    } catch (...) {
        rejectWithCurrentException(promise);
    }
}

template <class T> struct TaskReturn : TaskPromiseBase<T> {
    void return_value(T value) {
        settleOnIsolate(this->mPromise, [promise = this->mPromise, value = std::move(value)] {
            resolveOrReject(promise, value);
        });
    }
};
template <> struct TaskReturn<void> : TaskPromiseBase<void> {
    void return_void() {
        settleOnIsolate(this->mPromise, [promise = this->mPromise] { resolveOrReject(promise); });
    }
};

[[nodiscard]] inline auto describeReason(const Value& reason) -> std::string {
    if (!reason.valid()) {
        return "promise rejected";
    }
    const auto isolate = reason.isolate();
    const v8::HandleScope scope(isolate);
    const auto context = reason.creationContext(isolate);
    if (context.IsEmpty()) {
        return "promise rejected";
    }
    const v8::Context::Scope contextScope(context);
    const v8::String::Utf8Value utf8(isolate, reason.local(isolate));
    return *utf8 != nullptr ? std::string(*utf8, static_cast<std::size_t>(utf8.length()))
                            : std::string("promise rejected");
}

template <class U, bool Throwing> class JsAwaiter {
public:
    explicit JsAwaiter(Value awaited) : mAwaited(std::move(awaited)) {}
    JsAwaiter(const JsAwaiter&) = delete;
    JsAwaiter& operator=(const JsAwaiter&) = delete;

    bool await_ready() {
        if (!mAwaited.valid() || !mAwaited.state()->mLink->onIsolateThread()) {
            mError = ConvertError{"co_await on an empty, disposed or foreign-thread value"};
            return true;
        }
        const auto isolate = mAwaited.isolate();
        const v8::HandleScope scope(isolate);
        const auto context = mAwaited.creationContext(isolate);
        const v8::Context::Scope contextScope(context);
        ConvertCtx cx{isolate, context};
        const auto local = mAwaited.local(isolate);
        if (!local->IsPromise()) {
            mFulfilled = true;
            mOutcome = mAwaited;
            return true;
        }
        const auto promise = local.As<v8::Promise>();
        switch (promise->State()) {
        case v8::Promise::kPending:
            return false;
        case v8::Promise::kFulfilled:
            mFulfilled = true;
            mOutcome = Value(cx, promise->Result());
            return true;
        case v8::Promise::kRejected:
            promise->MarkAsHandled();
            mFulfilled = false;
            mOutcome = Value(cx, promise->Result());
            return true;
        }
        return false;
    }

    bool await_suspend(const std::coroutine_handle<> handle) {
        mHandle = handle;
        const auto isolate = mAwaited.isolate();
        const v8::HandleScope scope(isolate);
        const auto context = mAwaited.creationContext(isolate);
        const v8::Context::Scope contextScope(context);
        const auto data = Detail::newExternal(isolate, this, Detail::kAwaiterTag);
        v8::Local<v8::Function> onFulfilled;
        v8::Local<v8::Function> onRejected;
        mLink = isolateLink(isolate);
        mLink->trackSuspended(handle.address());
        if (!v8::Function::New(context, &onSettled<true>, data, 1).ToLocal(&onFulfilled) ||
            !v8::Function::New(context, &onSettled<false>, data, 1).ToLocal(&onRejected) ||
            mAwaited.local(isolate)
                .template As<v8::Promise>()
                ->Then(context, onFulfilled, onRejected)
                .IsEmpty()) {
            mLink->untrackSuspended(handle.address());
            mError = ConvertError{"co_await: V8 refused to subscribe to the promise "
                                  "(execution terminating?)"};
            return false;
        }
        return true;
    }

    auto await_resume() {
        if constexpr (Throwing) {
            auto result = outcome();
            if (!result) {
                throw JsException(std::move(result).error());
            }
            if constexpr (!std::is_void_v<U>) {
                return std::move(*result);
            }
        } else {
            return outcome();
        }
    }

private:
    template <bool Fulfilled>
    static void onSettled(const v8::FunctionCallbackInfo<v8::Value>& info) {
        auto* self = static_cast<JsAwaiter*>(
            externalValue(info.Data().template As<v8::External>(), kAwaiterTag));
        const auto isolate = info.GetIsolate();
        ConvertCtx cx{isolate, isolate->GetCurrentContext()};
        self->mFulfilled = Fulfilled;
        self->mOutcome =
            Value(cx, info.Length() > 0 ? info[0] : v8::Local<v8::Value>(v8::Undefined(isolate)));
        self->mLink->untrackSuspended(self->mHandle.address());
        self->mHandle.resume();
    }

    auto outcome() -> Result<U> {
        if (mError) {
            return std::unexpected(std::move(*mError));
        }
        if (!mFulfilled) {
            return std::unexpected(ConvertError{describeReason(mOutcome), mOutcome.state()});
        }
        if constexpr (std::is_void_v<U>) {
            return {};
        } else if constexpr (std::is_same_v<U, Value>) {
            return mOutcome;
        } else {
            return mOutcome.template to<U>();
        }
    }

    Value mAwaited;
    Value mOutcome;
    bool mFulfilled = false;
    std::optional<ConvertError> mError;
    std::coroutine_handle<> mHandle;
    std::shared_ptr<IsolateLink> mLink;
};

class OffloadResume {
public:
    OffloadResume(const std::coroutine_handle<> handle, std::shared_ptr<IsolateLink> link,
                  Value context)
        : mHandle(handle), mLink(std::move(link)), mContext(std::move(context)) {}
    OffloadResume(OffloadResume&& other) noexcept
        : mHandle(std::exchange(other.mHandle, {})), mLink(std::move(other.mLink)),
          mContext(std::move(other.mContext)) {}
    OffloadResume& operator=(OffloadResume&&) = delete;
    ~OffloadResume() {
        if (mHandle) {
            mContext = Value();
            mHandle.destroy();
            mLink->release();
        }
    }

    void operator()() {
        const auto handle = std::exchange(mHandle, {});
        mLink->release();
        const auto isolate = mLink->isolate();
        const v8::HandleScope scope(isolate);
        const auto context = mContext.creationContext(isolate);
        mContext = Value();
        if (context.IsEmpty()) {
            handle.resume();
            return;
        }
        const v8::Context::Scope contextScope(context);
        handle.resume();
    }

private:
    std::coroutine_handle<> mHandle;
    std::shared_ptr<IsolateLink> mLink;
    Value mContext;
};

class ResumeOnAwaiter {
public:
    explicit ResumeOnAwaiter(const Dispatcher& dispatcher) : mLink(dispatcher.link()) {}

    [[nodiscard]] bool await_ready() const { return !mLink || mLink->onIsolateThread(); }

    template <class P> void await_suspend(const std::coroutine_handle<P> handle) const {
        auto link = mLink;
        Value context;
        if constexpr (requires { handle.promise().mPromise; }) {
            context = handle.promise().mPromise;
        }
        link->retain();
        (void)link->postTask(OffloadResume(handle, link, std::move(context)));
    }

    void await_resume() const {
        if (!mLink) {
            throw JsException("Alka::resumeOn: the Dispatcher is empty");
        }
    }

private:
    std::shared_ptr<IsolateLink> mLink;
};

template <class F> class OffloadAwaiter {
public:
    using R = std::invoke_result_t<F&>;

    explicit OffloadAwaiter(F fn) : mFn(std::move(fn)) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    bool await_suspend(const std::coroutine_handle<> handle) {
        const auto isolate = v8::Isolate::TryGetCurrent();
        if (isolate == nullptr) {
            mError = std::make_exception_ptr(
                JsException("Alka::offload: co_await it on the isolate's thread"));
            return false;
        }
        auto link = isolateLink(isolate);
        Value context;
        if (isolate->InContext()) {
            const v8::HandleScope scope(isolate);
            const auto current = isolate->GetCurrentContext();
            ConvertCtx cx{isolate, current};
            context = Value(cx, current->Global());
        }
        link->retain();
        try {
            std::thread([this, handle, link, context = std::move(context)]() mutable {
                try {
                    if constexpr (std::is_void_v<R>) {
                        mFn();
                    } else {
                        mValue.emplace(mFn());
                    }
                } catch (...) {
                    mError = std::current_exception();
                }
                (void)link->postTask(OffloadResume(handle, link, std::move(context)));
            }).detach();
        } catch (...) {
            link->release();
            mError = std::current_exception();
            return false;
        }
        return true;
    }

    auto await_resume() -> R {
        if (mError) {
            std::rethrow_exception(mError);
        }
        if constexpr (!std::is_void_v<R>) {
            return std::move(*mValue);
        }
    }

private:
    F mFn;
    std::conditional_t<std::is_void_v<R>, bool, std::optional<R>> mValue{};
    std::exception_ptr mError;
};

} // namespace Detail

template <class T> class Task {
public:
    static constexpr bool alkaIsTask = true;
    using ValueType = T;

    struct promise_type : Detail::TaskReturn<T> {
        auto get_return_object() -> Task { return Task(this->mPromise); }
    };

    [[nodiscard]] auto promise() const -> const Promise<T>& { return mPromise; }

    auto operator co_await() const { return Detail::JsAwaiter<T, true>(mPromise); }

private:
    explicit Task(Promise<T> promise) : mPromise(std::move(promise)) {}

    Promise<T> mPromise;
};

template <class U = Value> [[nodiscard]] auto await(Value value) {
    return Detail::JsAwaiter<U, true>(std::move(value));
}

template <class U = Value> [[nodiscard]] auto tryAwait(Value value) {
    return Detail::JsAwaiter<U, false>(std::move(value));
}

template <class F>
    requires std::invocable<F&>
[[nodiscard]] auto offload(F fn) {
    return Detail::OffloadAwaiter<F>(std::move(fn));
}

[[nodiscard]] inline auto resumeOn(const Dispatcher& dispatcher) {
    return Detail::ResumeOnAwaiter(dispatcher);
}

template <class T> struct Converter<Task<T>> {
    static auto toJs(ConvertCtx& cx, const Task<T>& task) -> v8::Local<v8::Value> {
        return Converter<Promise<T>>::toJs(cx, task.promise());
    }
};

} // namespace Alka
