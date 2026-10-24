#pragma once

#include <Alka/Detail/IsolateLink.hpp>
#include <Alka/Detail/State.hpp>

#include <v8.h>

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

// Getting work onto an isolate's thread from anywhere.
//
//   auto dispatcher = Alka::Dispatcher(isolate);          // on the isolate thread
//   dispatcher.setWakeup([loop] { loop->wake(); });       // e.g. uv_async_send
//   std::thread([dispatcher, settler] {                   // any thread
//       auto bytes = slowRead();
//       dispatcher.post([settler, bytes] { settler.resolve(bytes); });
//   }).detach();
//   ...
//   dispatcher.drain();                                   // host loop, isolate thread

namespace Alka {

class Dispatcher {
public:
    Dispatcher() = default;
    explicit Dispatcher(v8::Isolate* isolate) : mLink(Detail::isolateLink(isolate)) {}

    template <class F>
        requires std::invocable<F&>
    bool post(F&& task) const {
        return mLink && mLink->postTask(Detail::UniqueFn(std::forward<F>(task)));
    }

    [[nodiscard]] auto drain(std::size_t max = (std::numeric_limits<std::size_t>::max)()) const
        -> std::size_t;

    void setWakeup(std::function<void()> wakeup) const {
        if (mLink) {
            mLink->setWakeup(std::move(wakeup));
        }
    }

    [[nodiscard]] bool busy() const { return mLink && mLink->alive() && mLink->busy(); }

    void setActivityHook(std::function<void()> hook) const {
        if (mLink) {
            mLink->setActivityHook(std::move(hook));
        }
    }

    void retain() const noexcept {
        if (mLink) {
            mLink->retain();
        }
    }
    void release() const noexcept {
        if (mLink) {
            mLink->release();
        }
    }

    [[nodiscard]] bool alive() const noexcept { return mLink && mLink->alive(); }
    explicit operator bool() const noexcept { return mLink != nullptr; }

    [[nodiscard]] auto link() const noexcept -> const std::shared_ptr<Detail::IsolateLink>& {
        return mLink;
    }

private:
    std::shared_ptr<Detail::IsolateLink> mLink;
};

} // namespace Alka
