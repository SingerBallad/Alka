#pragma once

#include <Alka/Converter.hpp>
#include <Alka/Function.hpp>
#include <Alka/Holder.hpp>
#include <Alka/Install.hpp>

#include <functional>
#include <utility>

namespace Alka {

namespace Detail {

template<class Sig>
struct CallableHolder final : Holder {
    std::function<Sig> mFn;

    explicit CallableHolder(std::function<Sig> fn) : Holder(this), mFn(std::move(fn)) {}
};

template<class R, class... Args>
void callableTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
    static const CallSite kSite{"native callback", {}};
    auto* holder =
        static_cast<CallableHolder<R(Args...)>*>(info.Data().As<v8::External>()->Value());
    const auto isolate = info.GetIsolate();
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};
    auto args = convertArguments<std::tuple<Args...>>(
        info, cx, kSite, kNoDesc, std::index_sequence_for<Args...>{});
    if (!args) {
        return;
    }
    applyAndReturn<R>(info, cx, kSite, [&]() -> decltype(auto) {
        return std::apply(
            [&]<typename... T0>(T0&&... a) -> decltype(auto) {
                return holder->mFn(std::forward<T0>(a)...);
            },
            std::move(*args));
    });
}

}  // namespace Detail

template<class R, class... Args>
struct Converter<std::function<R(Args...)>> {
    static auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value)
        -> Result<std::function<R(Args...)>> {
        auto fn = Converter<Function<R(Args...)>>::fromJs(cx, value);
        if (!fn) {
            return std::unexpected(std::move(fn).error());
        }
        return std::function<R(Args...)>(
            [f = std::move(*fn)](Args... args) -> R { return f.call(std::move(args)...); });
    }

    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const std::function<R(Args...)>& value) {
        if (!value) {
            return v8::Undefined(cx.mIsolate);
        }
        auto* holder = new Detail::CallableHolder<R(Args...)>(value);
        v8::Local<v8::Function> jsFn =
            v8::Function::New(cx.mContext, &Detail::callableTrampoline<R, Args...>,
                              v8::External::New(cx.mIsolate, holder))
                .ToLocalChecked();
        Detail::trackExternalHolder(cx.mIsolate, jsFn, holder);
        return jsFn;
    }
};

}  // namespace Alka
