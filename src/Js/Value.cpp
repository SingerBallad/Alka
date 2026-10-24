#include <Alka/Detail/State.hpp>
#include <Alka/Js/Value.hpp>

#include <memory>

namespace Alka::Detail {

namespace {

void releaseValueState(ValueState* state) {
    const auto link = state->mLink;
    if (!link->canReleaseHere() && link->post([state] { delete state; })) {
        return;
    }
    delete state;
}

} // namespace

ValueState::~ValueState() {
    if (mLink) {
        mLink->untrack(this);
    }
    mValue.Reset();
    mContext.Reset();
}

auto ValueState::make(v8::Isolate* isolate, const v8::Local<v8::Context> context,
                      const v8::Local<v8::Value> value, const bool weak)
    -> std::shared_ptr<ValueState> {
    auto* state = new ValueState;
    state->mLink = isolateLink(isolate);
    state->mValue.Reset(isolate, value);
    state->mContext.Reset(isolate, context);
    if (weak) {
        state->mValue.SetWeak();
        state->mContext.SetWeak();
    }
    state->mLink->track(state);
    return {state, &releaseValueState};
}

auto caughtError(v8::Isolate* isolate, const v8::Local<v8::Context> context, v8::TryCatch& tryCatch)
    -> ConvertError {
    if (tryCatch.HasTerminated()) {
        tryCatch.ReThrow();
        return ConvertError{"JS execution was terminated"};
    }
    std::shared_ptr<ValueState> thrown;
    if (tryCatch.HasCaught()) {
        thrown = ValueState::make(isolate, context, tryCatch.Exception());
    }
    return ConvertError{tryCatchMessage(isolate, context, tryCatch), std::move(thrown)};
}

} // namespace Alka::Detail
