#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Core/Traits.hpp>
#include <Alka/Detail/Holder.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Js/Function.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Alka {

namespace Detail {

template <class F> struct RuntimeFunction {
    F mFn;
    CallSite mSite;
};

template <class F> void runtimeFunctionTrampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    auto& function = *static_cast<RuntimeFunction<F>*>(
        static_cast<Holder*>(externalValue(info.Data().As<v8::External>(), kNativeFunctionTag))
            ->mPtr);
    using Traits = CallableTraitsOf<F>;
    using Args = Traits::ArgsTuple;
    using R = Traits::Ret;
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};
    (void)invokeWith<Args, Dispatch::Single>(info, cx, function.mSite, kNoDesc, [&](auto& stored) {
        guarded(isolate, function.mSite, [&] {
            returnResult<R, ReturnPolicy::Automatic>(info, cx, [&]() -> decltype(auto) {
                return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> decltype(auto) {
                    return function.mFn(ArgTraits<std::tuple_element_t<Is, Args>>::forward(
                        std::get<Is>(stored))...);
                }(std::make_index_sequence<std::tuple_size_v<Args>>{});
            });
        });
    });
}

template <class F>
[[nodiscard]] auto runtimeFunctionToJs(ConvertCtx& cx,
                                       const std::shared_ptr<RuntimeFunction<F>>& function)
    -> v8::Local<v8::Value> {
    using Args = typename CallableTraitsOf<F>::ArgsTuple;
    auto holder = Holder::owned(function, function.get());
    const auto data = newExternal(cx.mIsolate, holder.get(), kNativeFunctionTag);
    v8::Local<v8::Function> jsFn;
    if (!v8::Function::New(cx.mContext, &runtimeFunctionTrampoline<F>, data,
                           static_cast<int>(minJsArity<NoDesc, Args>()),
                           v8::ConstructorBehavior::kThrow)
             .ToLocal(&jsFn)) {
        return v8::Undefined(cx.mIsolate); // an exception (termination) is pending
    }
    jsFn->SetName(v8Str(cx.mIsolate, function->mSite.mDisplayName));
    trackExternalHolder(cx.mIsolate, jsFn, std::move(holder));
    return jsFn;
}

} // namespace Detail

// (void)Alka::Object::global(cx).set(
//     "now", Alka::callable([&clock] { return clock.now(); }, "now"));
template <class F> class NativeFunction {
public:
    NativeFunction() = default;
    explicit NativeFunction(std::shared_ptr<Detail::RuntimeFunction<F>> function)
        : mFunction(std::move(function)) {}

    [[nodiscard]] auto function() const noexcept
        -> const std::shared_ptr<Detail::RuntimeFunction<F>>& {
        return mFunction;
    }

private:
    std::shared_ptr<Detail::RuntimeFunction<F>> mFunction;
};

template <class F>
[[nodiscard]] auto callable(F fn, const std::string_view name = "native function")
    -> NativeFunction<std::decay_t<F>> {
    using D = std::decay_t<F>;
    (void)sizeof(CallableTraitsOf<D>);
    Detail::CallSite site;
    site.mDisplayName = std::string(name);
    site.mKind = Detail::CallSite::Kind::Call;
    return NativeFunction<D>(std::make_shared<Detail::RuntimeFunction<D>>(
        Detail::RuntimeFunction<D>{std::move(fn), site}));
}

template <class F> struct Converter<NativeFunction<F>> {
    static auto toJs(ConvertCtx& cx, const NativeFunction<F>& value) -> v8::Local<v8::Value> {
        if (!value.function()) {
            return v8::Undefined(cx.mIsolate);
        }
        return Detail::runtimeFunctionToJs(cx, value.function());
    }
};

namespace Detail {

inline const CallSite kNativeCallbackSite{"native callback", {}, CallSite::Kind::Call};

} // namespace Detail

template <class R, class... Args> struct Converter<std::function<R(Args...)>> {
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
        using F = std::function<R(Args...)>;
        return Detail::runtimeFunctionToJs(
            cx, std::make_shared<Detail::RuntimeFunction<F>>(
                    Detail::RuntimeFunction<F>{value, Detail::kNativeCallbackSite}));
    }
};

} // namespace Alka
