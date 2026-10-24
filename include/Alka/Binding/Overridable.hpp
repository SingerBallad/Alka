#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Core/Error.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Js/Value.hpp>

#include <v8.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// C++ virtual functions JS classes can override.
//
//   class Animal { public: virtual ~Animal() = default;
//                  virtual std::string speak() { return "..."; } };
//
//   // the trampoline: forwards each virtual to a JS override, if any
//   struct JsAnimal : Animal, Alka::Overridable<Animal> {
//       using Animal::Animal;
//       std::string speak() override { return ALKA_OVERRIDE(std::string, Animal, speak); }
//   };
//
//   template<> struct Alka::Bind<Animal> : Alka::Class<Animal> {
//       using Trampoline = JsAnimal;
//       static constexpr auto members = Alka::members(Alka::ctor<>(),
//                                                     Alka::method<&Animal::speak>());
//   };
//
//   // JS:  class Dog extends zoo.Animal { speak() { return "woof, " + super.speak(); } }
//   //      zoo.chorus(new Dog())   // C++ calls animal->speak() -> "woof, ..."
//
// JS-constructed instances are trampolines; C++ calling a virtual on one runs
// the JS override (or the C++ base when there is none -- directly, without a
// trip through JS -- and for super calls, never recursing). A JS exception
// thrown by an override propagates as Alka::JsException. Overrides run only on
// the isolate thread (elsewhere the C++ base runs). Keep the object alive from
// C++ through a shared_ptr obtained from Alka (a parameter, Alka::pin): that
// also keeps its JS side -- the overrides -- alive.

namespace Alka {

namespace Detail {

template <class A>
[[nodiscard]] auto overrideArgToJs(ConvertCtx& cx, A&& arg) -> v8::Local<v8::Value> {
    using D = std::remove_cvref_t<A>;
    if constexpr (std::is_lvalue_reference_v<A> && BoundClass<D>) {
        return wrapBorrowed(cx, std::addressof(arg));
    } else if constexpr (std::is_pointer_v<D> &&
                         BoundClass<std::remove_cv_t<std::remove_pointer_t<D>>>) {
        return arg == nullptr ? v8::Local<v8::Value>(v8::Null(cx.mIsolate)) : wrapBorrowed(cx, arg);
    } else {
        return toJs(cx, arg);
    }
}

} // namespace Detail

template <class Base> class Overridable {
public:
    void alkaAttach(v8::Isolate* isolate, const v8::Local<v8::Context> context,
                    const v8::Local<v8::Object> self, const void* object,
                    NameText (*const names)(std::string_view)) {
        mAlkaNames = names;
        if (mAlkaSelf && !mAlkaSelf->mDead && !mAlkaSelf->mValue.IsEmpty() &&
            mAlkaSelf->mLink->isolate() == isolate) {
            return;
        }
        mAlkaSelf = Detail::ValueState::make(isolate, context, self, true);
        mAlkaObject = object;
    }

    template <class R, FixedString CxxName, class Fallback, class... A>
    R alkaOverride(Fallback&& base, A&&... args) const {
        if (mAlkaNames == nullptr) {
            return std::forward<Fallback>(base)();
        }
        static const NameText jsName = mAlkaNames(CxxName.view());
        return dispatchOverride<R>(
            jsName.view(),
            [](v8::Isolate* isolate) {
                return Detail::hotKey(isolate, Detail::hotKeySlot<OverrideKey<CxxName>>(),
                                      jsName.view());
            },
            std::forward<Fallback>(base), std::forward<A>(args)...);
    }

    template <class R, FixedString JsName, class Fallback, class... A>
    R alkaOverrideJs(Fallback&& base, A&&... args) const {
        return dispatchOverride<R>(
            JsName.view(), [](v8::Isolate* isolate) { return Detail::cachedKey<JsName>(isolate); },
            std::forward<Fallback>(base), std::forward<A>(args)...);
    }

    template <class R, class Fallback, class... A>
    R alkaOverride(const std::string_view jsName, Fallback&& base, A&&... args) const {
        return dispatchOverride<R>(
            jsName, [jsName](v8::Isolate* isolate) { return Detail::v8Str(isolate, jsName, true); },
            std::forward<Fallback>(base), std::forward<A>(args)...);
    }

protected:
    Overridable() = default;
    Overridable(const Overridable&) noexcept {}
    Overridable& operator=(const Overridable&) noexcept { return *this; }
    ~Overridable() = default;

private:
    template <FixedString CxxName> struct OverrideKey {};

    template <class R, class KeyOf, class Fallback, class... A>
    R dispatchOverride(const std::string_view jsName, const KeyOf keyOf, Fallback&& base,
                       A&&... args) const {
        if (Detail::tBaseCall.mObject == mAlkaObject && mAlkaObject != nullptr &&
            Detail::tBaseCall.mName == jsName) {
            Detail::tBaseCall = {};
            return std::forward<Fallback>(base)();
        }
        const auto& state = mAlkaSelf;
        if (!state || state->mDead || state->mValue.IsEmpty() || state->mContext.IsEmpty() ||
            !state->mLink->onIsolateThread()) {
            return std::forward<Fallback>(base)();
        }
        const auto isolate = state->mLink->isolate();
        const v8::HandleScope scope(isolate);
        const auto context = state->mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);
        const auto self = state->mValue.Get(isolate).As<v8::Object>();

        v8::TryCatch tryCatch(isolate);
        v8::Local<v8::Value> method;
        if (!self->Get(context, keyOf(isolate)).ToLocal(&method)) {
            throw JsException(Detail::caughtError(isolate, context, tryCatch));
        }
        if (!method->IsFunction()) {
            return std::forward<Fallback>(base)();
        }
        if (method.As<v8::Object>()
                ->HasPrivate(context, Detail::cachedPrivate<"Alka::nativeMethod">(isolate))
                .FromMaybe(false)) {
            return std::forward<Fallback>(base)();
        }
        ConvertCtx cx{isolate, context};
        const Detail::NativeCallScope nativeFrame;
        std::array<v8::Local<v8::Value>, sizeof...(A)> argv{
            Detail::overrideArgToJs(cx, std::forward<A>(args))...};
        v8::Local<v8::Value> result;
        if (!method.As<v8::Function>()
                 ->Call(context, self, static_cast<int>(argv.size()), argv.data())
                 .ToLocal(&result)) {
            throw JsException(Detail::caughtError(isolate, context, tryCatch));
        }
        if constexpr (!std::is_void_v<R>) {
            auto converted = fromJs<R>(cx, result);
            if (!converted) {
                throw JsException(ConvertError{
                    "the JS override of " + std::string(jsName) +
                        " returned an incompatible value: " + converted.error().message(),
                    std::move(converted.error().mJsException)});
            }
            return std::move(*converted);
        }
    }

    std::shared_ptr<Detail::ValueState> mAlkaSelf;
    const void* mAlkaObject = nullptr;
    NameText (*mAlkaNames)(std::string_view) = nullptr;
};

template <class R, FixedString Name, class Self, class Fallback, class... A>
R overrideOr(Self* self, Fallback&& base, A&&... args) {
    return self->template alkaOverrideJs<R, Name>(std::forward<Fallback>(base),
                                                  std::forward<A>(args)...);
}

template <class R, class Self, class Fallback, class... A>
R overrideOr(Self* self, const std::string_view jsName, Fallback&& base, A&&... args) {
    return self->template alkaOverride<R>(jsName, std::forward<Fallback>(base),
                                          std::forward<A>(args)...);
}

} // namespace Alka

#define ALKA_OVERRIDE(Ret, Base, name, ...)                                                        \
    this->template alkaOverride<Ret, #name>(                                                       \
        [&]() -> Ret { return Base::name(__VA_ARGS__); } __VA_OPT__(, ) __VA_ARGS__)

#define ALKA_OVERRIDE_PURE(Ret, Base, name, ...)                                                   \
    this->template alkaOverride<Ret, #name>([&]() -> Ret {                                         \
        throw ::Alka::JsException(std::string(#Base "." #name " is not implemented"));             \
    } __VA_OPT__(, ) __VA_ARGS__)
