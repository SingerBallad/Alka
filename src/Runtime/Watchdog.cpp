#include <Alka/Runtime/Watchdog.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Alka {

struct Watchdog::Impl {
    v8::Isolate* mIsolate = nullptr;
    std::mutex mMutex;
    std::condition_variable mWake;
    bool mDone = false;
    std::atomic<bool> mFired{false};
    std::thread mThread;
};

Watchdog::Watchdog(v8::Isolate* isolate, const std::chrono::milliseconds limit)
    : mImpl(std::make_unique<Impl>()) {
    mImpl->mIsolate = isolate;
    mImpl->mThread = std::thread([impl = mImpl.get(), limit] {
        std::unique_lock lock(impl->mMutex);
        if (!impl->mWake.wait_for(lock, limit, [impl] { return impl->mDone; })) {
            impl->mFired.store(true);
            impl->mIsolate->TerminateExecution();
        }
    });
}

Watchdog::~Watchdog() {
    {
        const std::lock_guard lock(mImpl->mMutex);
        mImpl->mDone = true;
    }
    mImpl->mWake.notify_all();
    mImpl->mThread.join();
    if (mImpl->mFired.load()) {
        mImpl->mIsolate->CancelTerminateExecution();
    }
}

bool Watchdog::fired() const noexcept {
    return mImpl->mFired.load();
}

} // namespace Alka
