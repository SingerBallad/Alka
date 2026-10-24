#include <Alka/Detail/IsolateLink.hpp>
#include <Alka/Js/Value.hpp>

#include <coroutine>
#include <mutex>
#include <utility>
#include <vector>

namespace Alka::Detail {

bool IsolateLink::enqueue(std::vector<UniqueFn>* maintenance, std::deque<UniqueFn>* tasks,
                          UniqueFn fn) {
    std::function<void()> wakeup;
    {
        const std::lock_guard lock(mMutex);
        if (!mAlive.load(std::memory_order_relaxed)) {
            return false;
        }
        const bool wasIdle = mPending.empty() && mTasks.empty();
        if (maintenance != nullptr) {
            maintenance->push_back(std::move(fn));
            mHasPending.store(true, std::memory_order_release);
        } else {
            tasks->push_back(std::move(fn));
        }
        if (wasIdle) {
            wakeup = mWakeup;
        }
    }
    if (wakeup) {
        wakeup();
    }
    return true;
}

bool IsolateLink::post(UniqueFn fn) {
    return enqueue(&mPending, nullptr, std::move(fn));
}

bool IsolateLink::postTask(UniqueFn fn) {
    return enqueue(nullptr, &mTasks, std::move(fn));
}

void IsolateLink::runPending() {
    std::vector<UniqueFn> work;
    {
        const std::lock_guard lock(mMutex);
        work.swap(mPending);
        mHasPending.store(false, std::memory_order_release);
    }
    for (auto& fn : work) {
        fn();
    }
}

auto IsolateLink::runTasks(const std::size_t max) -> std::size_t {
    std::size_t ran = 0;
    while (ran < max) {
        UniqueFn task;
        {
            const std::lock_guard lock(mMutex);
            if (mTasks.empty() || !mAlive.load(std::memory_order_relaxed)) {
                break;
            }
            task = std::move(mTasks.front());
            mTasks.pop_front();
        }
        task();
        ++ran;
    }
    return ran;
}

void IsolateLink::setWakeup(std::function<void()> wakeup) {
    const std::lock_guard lock(mMutex);
    mWakeup = std::move(wakeup);
}

void IsolateLink::setActivityHook(std::function<void()> hook) {
    const std::lock_guard lock(mMutex);
    mActivity = std::move(hook);
}

void IsolateLink::notifyActivity() noexcept {
    std::function<void()> hook;
    {
        const std::lock_guard lock(mMutex);
        hook = mActivity;
    }
    if (hook) {
        hook();
    }
}

void IsolateLink::track(ValueState* state) {
    const std::lock_guard lock(mMutex);
    mStates.insert(state);
}

void IsolateLink::untrack(ValueState* state) {
    const std::lock_guard lock(mMutex);
    mStates.erase(state);
}

void IsolateLink::trackSuspended(void* frame) {
    const std::lock_guard lock(mMutex);
    mSuspended.insert(frame);
}

void IsolateLink::untrackSuspended(void* frame) {
    const std::lock_guard lock(mMutex);
    mSuspended.erase(frame);
}

void IsolateLink::shutdown() {
    std::vector<UniqueFn> maintenance;
    std::deque<UniqueFn> tasks;
    std::unordered_set<void*> suspended;
    {
        const std::lock_guard lock(mMutex);
        for (auto* state : mStates) {
            state->mValue.Reset();
            state->mContext.Reset();
            state->mDead.store(true, std::memory_order_release);
        }
        mAlive.store(false, std::memory_order_release);
        maintenance.swap(mPending);
        tasks.swap(mTasks);
        suspended.swap(mSuspended);
        mHasPending.store(false, std::memory_order_release);
        mWakeup = nullptr;
        mActivity = nullptr;
    }
    for (auto& fn : maintenance) {
        fn();
    }
    tasks.clear();
    for (void* frame : suspended) {
        std::coroutine_handle<>::from_address(frame).destroy();
    }
}

} // namespace Alka::Detail
