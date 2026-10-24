#include <Alka/Runtime/Dispatcher.hpp>
#include <Alka/Runtime/Lifetime.hpp>

namespace Alka {

auto Dispatcher::drain(const std::size_t max) const -> std::size_t {
    if (!mLink || !mLink->alive() || !mLink->onIsolateThread()) {
        return 0;
    }
    const auto isolate = mLink->isolate();
    const v8::Isolate::Scope isolateScope(isolate);
    mLink->runPending();
    std::size_t ran = 0;
    while (ran < max) {
        const v8::HandleScope scope(isolate);
        if (mLink->runTasks(1) == 0) {
            break;
        }
        ++ran;
    }
    (void)runDeferredDestruction(isolate);
    return ran;
}

} // namespace Alka
