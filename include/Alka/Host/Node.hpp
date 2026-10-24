#pragma once

#include <Alka/Detail/State.hpp>
#include <Alka/Runtime/Dispatcher.hpp>

#include <node.h>
#include <uv.h>
#include <v8.h>

#include <map>
#include <memory>
#include <mutex>
#include <thread>

// NODE_MODULE_INIT(/* exports, module, context */) {
//     Alka::Node::attach(context);   // first, once per environment
//     Alka::install<Geometry>(context);
//     ...
// }

namespace Alka::Node {

namespace Detail {

struct Loop {
    v8::Isolate* mIsolate = nullptr;
    v8::Global<v8::Context> mContext;
    Dispatcher mDispatcher;
    std::thread::id mThread; // the environment's (loop) thread
    uv_async_t mAsync{};
    bool mReferenced = false; // loop thread
    std::mutex mMutex;
    bool mClosed = false;        // (mMutex) the handle is closing
    std::shared_ptr<Loop> mSelf; // until the handle is closed
    node::AsyncCleanupHookHandle mCleanupHook;
    void (*mCleanupDone)(void*) = nullptr;
    void* mCleanupDoneArg = nullptr;

    [[nodiscard]] auto handle() noexcept -> uv_handle_t* {
        return reinterpret_cast<uv_handle_t*>(&mAsync);
    }

    void wake() {
        const std::lock_guard lock(mMutex);
        if (!mClosed) {
            uv_async_send(&mAsync);
        }
    }

    void updateReference() {
        if (mClosed) {
            return;
        }
        if (const bool busy = mDispatcher.busy(); busy != mReferenced) {
            mReferenced = busy;
            busy ? uv_ref(handle()) : uv_unref(handle());
        }
    }
};

inline std::mutex gLoopsMutex;
inline std::map<v8::Isolate*, Loop*> gLoops;

inline void drainOnLoop(uv_async_t* async) {
    auto& loop = *static_cast<Loop*>(async->data);
    const auto isolate = loop.mIsolate;
    {
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        const auto context = loop.mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);
        const node::CallbackScope callbackScope(isolate, v8::Object::New(isolate), {0, 0});
        (void)loop.mDispatcher.drain();
    }
    loop.updateReference();
}

inline void closeLoop(void* arg, void (*done)(void*), void* doneArg) {
    auto* loop = static_cast<Loop*>(arg);
    {
        const std::lock_guard lock(gLoopsMutex);
        gLoops.erase(loop->mIsolate);
    }
    loop->mDispatcher.setWakeup(nullptr);
    loop->mDispatcher.setActivityHook(nullptr);
    {
        const v8::Isolate::Scope isolateScope(loop->mIsolate);
        const v8::HandleScope handleScope(loop->mIsolate);
        loop->mContext.Reset();
        Alka::disposeIsolate(loop->mIsolate);
    }
    {
        const std::lock_guard lock(loop->mMutex);
        loop->mClosed = true;
    }
    loop->mCleanupDone = done;
    loop->mCleanupDoneArg = doneArg;
    uv_close(loop->handle(), [](uv_handle_t* handle) {
        auto* closing = static_cast<Loop*>(handle->data);
        const auto finished = closing->mCleanupDone;
        const auto finishedArg = closing->mCleanupDoneArg;
        const auto self = std::move(closing->mSelf);
        finished(finishedArg);
    });
}

} // namespace Detail

inline void attach(const v8::Local<v8::Context> context) {
    const auto isolate = v8::Isolate::GetCurrent();
    {
        const std::lock_guard lock(Detail::gLoopsMutex);
        if (Detail::gLoops.contains(isolate)) {
            return;
        }
    }
    auto loop = std::make_shared<Detail::Loop>();
    loop->mIsolate = isolate;
    loop->mContext.Reset(isolate, context);
    loop->mDispatcher = Dispatcher(isolate);
    loop->mThread = std::this_thread::get_id();
    loop->mSelf = loop;
    uv_async_init(node::GetCurrentEventLoop(isolate), &loop->mAsync, &Detail::drainOnLoop);
    loop->mAsync.data = loop.get();
    uv_unref(loop->handle());
    loop->mDispatcher.setWakeup([loop] { loop->wake(); });
    loop->mDispatcher.setActivityHook([loop] {
        if (std::this_thread::get_id() == loop->mThread) {
            loop->updateReference();
        } else {
            loop->wake();
        }
    });
    loop->mCleanupHook = node::AddEnvironmentCleanupHook(isolate, &Detail::closeLoop, loop.get());
    {
        const std::lock_guard lock(Detail::gLoopsMutex);
        Detail::gLoops.emplace(isolate, loop.get());
    }
    loop->updateReference();
}

} // namespace Alka::Node
