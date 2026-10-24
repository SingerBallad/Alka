#pragma once

#include <v8.h>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Alka::Detail {

class UniqueFn {
public:
    UniqueFn() = default;
    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, UniqueFn>) && std::invocable<F&>
    UniqueFn(F&& fn) : mImpl(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(fn))) {}

    explicit operator bool() const noexcept { return mImpl != nullptr; }
    void operator()() const { mImpl->call(); }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void call() = 0;
    };
    template <class F> struct Model final : Concept {
        explicit Model(F fn) : mFn(std::move(fn)) {}
        void call() override { mFn(); }
        F mFn;
    };
    std::unique_ptr<Concept> mImpl;
};

struct ValueState;

class IsolateLink {
public:
    explicit IsolateLink(v8::Isolate* isolate)
        : mIsolate(isolate), mHomeThread(std::this_thread::get_id()),
          mUsesLocker(v8::Locker::IsLocked(isolate)) {}

    [[nodiscard]] auto isolate() const noexcept -> v8::Isolate* { return mIsolate; }
    [[nodiscard]] bool alive() const noexcept { return mAlive.load(std::memory_order_acquire); }

    [[nodiscard]] bool onIsolateThread() const {
        if (v8::Isolate::TryGetCurrent() == mIsolate) {
            return true;
        }
        if (!alive()) {
            return false;
        }
        if (v8::Locker::IsLocked(mIsolate)) {
            return true;
        }
        return !mUsesLocker.load(std::memory_order_acquire) &&
               std::this_thread::get_id() == mHomeThread;
    }

    [[nodiscard]] bool canReleaseHere() const noexcept {
        if (v8::Isolate::TryGetCurrent() == mIsolate) {
            return true;
        }
        return !mUsesLocker.load(std::memory_order_acquire) &&
               std::this_thread::get_id() == mHomeThread;
    }

    void noteLocker() {
        if (!mUsesLocker.load(std::memory_order_relaxed) && v8::Locker::IsLocked(mIsolate)) {
            mUsesLocker.store(true, std::memory_order_release);
        }
    }

    bool post(UniqueFn fn);
    bool postTask(UniqueFn fn);
    void runPending();
    auto runTasks(std::size_t max) -> std::size_t;

    [[nodiscard]] bool hasPending() const noexcept {
        return mHasPending.load(std::memory_order_acquire);
    }

    void setWakeup(std::function<void()> wakeup);

    void track(ValueState* state);
    void untrack(ValueState* state);

    void retain() noexcept {
        if (mKeepAlive.fetch_add(1, std::memory_order_acq_rel) == 0) {
            notifyActivity();
        }
    }
    void release() noexcept {
        if (mKeepAlive.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            notifyActivity();
        }
    }

    void setActivityHook(std::function<void()> hook);

    [[nodiscard]] bool busy() {
        if (mKeepAlive.load(std::memory_order_acquire) > 0) {
            return true;
        }
        const std::lock_guard lock(mMutex);
        return !mTasks.empty() || !mPending.empty();
    }

    void trackSuspended(void* frame);
    void untrackSuspended(void* frame);

    void shutdown();

    std::mutex mMutex;

private:
    bool enqueue(std::vector<UniqueFn>* maintenance, std::deque<UniqueFn>* tasks, UniqueFn fn);
    void notifyActivity() noexcept;

    v8::Isolate* const mIsolate;
    std::thread::id mHomeThread;
    std::atomic<bool> mAlive{true};
    std::atomic<bool> mHasPending{false};
    std::atomic<int> mKeepAlive{0};
    std::atomic<bool> mUsesLocker{false};
    std::vector<UniqueFn> mPending;
    std::deque<UniqueFn> mTasks;
    std::function<void()> mWakeup;
    std::function<void()> mActivity;
    std::unordered_set<ValueState*> mStates;
    std::unordered_set<void*> mSuspended;
};

} // namespace Alka::Detail
