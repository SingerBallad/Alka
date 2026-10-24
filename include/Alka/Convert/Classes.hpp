#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Detail/Invoke.hpp>

#include <memory>
#include <type_traits>

namespace Alka {

template <BoundClass T>
    requires std::copy_constructible<T>
struct Converter<T> {
    static auto fromJs(ConvertCtx&, v8::Local<v8::Value> value) -> Result<T> {
        auto unwrapped = Detail::unwrapObject<T>(value);
        if (!unwrapped) {
            return std::unexpected(std::move(unwrapped).error());
        }
        return T(*unwrapped->mPtr);
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const T& value) {
        return Detail::wrapOwning(cx, std::make_shared<T>(value));
    }
};

template <class T>
    requires BoundClass<std::remove_const_t<T>>
struct Converter<std::shared_ptr<T>> {
    using U = std::remove_const_t<T>;
    static_assert(!isGcClass<U>, "Alka: a C++-heap (GcObject) class is not shared through a "
                                 "shared_ptr: use T*, T& or cppgc::Persistent<T>");

    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value)
        -> Result<std::shared_ptr<T>> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return std::shared_ptr<T>{};
        }
        auto unwrapped = Detail::unwrapObject<U>(value);
        if (!unwrapped) {
            return std::unexpected(std::move(unwrapped).error());
        }
        std::shared_ptr<void> owner = Detail::pinnedOwner(*unwrapped->mHolder);
        if (!owner) {
            return std::unexpected(ConvertError{
                "this " + std::string(Detail::kTypeNode<U>.mName) +
                (unwrapped->mHolder->isPath()
                     ? " is a live view into another object, so it cannot be shared (copy it)"
                     : " is borrowed from C++, so it cannot be shared (C++ did not hand out "
                       "ownership)")});
        }
        return std::shared_ptr<T>(std::move(owner), unwrapped->mPtr); // aliasing ctor
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const std::shared_ptr<T>& value) {
        if (!value) {
            return v8::Null(cx.mIsolate);
        }
        return Detail::wrapOwning(cx, value);
    }
};

template <class T>
    requires BoundClass<std::remove_const_t<T>>
struct Converter<std::unique_ptr<T>> {
    template <class Dependent = T>
    static auto fromJs(ConvertCtx&, v8::Local<v8::Value>) -> Result<std::unique_ptr<T>> {
        static_assert(Detail::AlwaysFalse<Dependent>,
                      "Alka: a std::unique_ptr<T> parameter would take sole ownership, which JS "
                      "cannot give (what JS made lives in a shared_ptr): take std::shared_ptr<T> "
                      "(C++ co-owns it, and keeps its JS wrapper alive) or T& / T* (borrows it)");
        return std::unexpected(ConvertError{""});
    }
    static auto toJs(ConvertCtx& cx, std::unique_ptr<T>&& value) -> v8::Local<v8::Value> {
        if (!value) {
            return v8::Null(cx.mIsolate);
        }
        return Detail::wrapOwning(cx, std::shared_ptr<T>(std::move(value)));
    }
};

template <class T>
    requires BoundClass<std::remove_const_t<T>>
struct Converter<std::weak_ptr<T>> {
    using U = std::remove_const_t<T>;
    static_assert(!isGcClass<U>, "Alka: a C++-heap (GcObject) class is not observed through a "
                                 "weak_ptr: use cppgc::WeakMember<T> or cppgc::WeakPersistent<T>");

    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<std::weak_ptr<T>> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return std::weak_ptr<T>{};
        }
        auto unwrapped = Detail::unwrapObject<U>(value);
        if (!unwrapped) {
            return std::unexpected(std::move(unwrapped).error());
        }
        std::shared_ptr<void> owner = unwrapped->mHolder->shareOwnership();
        if (!owner) {
            return std::unexpected(ConvertError{
                "this " + std::string(Detail::kTypeNode<U>.mName) +
                (unwrapped->mHolder->isPath()
                     ? " is a live view into another object, so it cannot be observed through a "
                       "weak_ptr"
                     : " is borrowed from C++, so it cannot be observed through a weak_ptr")});
        }
        return std::weak_ptr<T>(std::shared_ptr<T>(std::move(owner), unwrapped->mPtr));
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const std::weak_ptr<T>& value) {
        return Detail::wrapWeak(cx, value);
    }
};

template <class T> struct Borrowed {
    T* mPtr = nullptr;
};

template <class T>
    requires BoundClass<std::remove_cv_t<T>>
[[nodiscard]] constexpr auto borrow(T* ptr) noexcept -> Borrowed<T> {
    return Borrowed<T>{ptr};
}

template <class T>
    requires BoundClass<std::remove_cv_t<T>>
struct Converter<Borrowed<T>> {
    static auto toJs(ConvertCtx& cx, const Borrowed<T>& value) -> v8::Local<v8::Value> {
        if (value.mPtr == nullptr) {
            return v8::Null(cx.mIsolate);
        }
        return Detail::lendToJs<true>(cx, value.mPtr);
    }
};

template <class T>
    requires BoundClass<std::remove_cv_t<T>>
struct Converter<T*> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<T*> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return nullptr;
        }
        auto unwrapped = Detail::unwrapObject<std::remove_cv_t<T>>(value);
        if (!unwrapped) {
            return std::unexpected(std::move(unwrapped).error());
        }
        return unwrapped->mPtr;
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, T* const& value) {
        if (value == nullptr) {
            return v8::Null(cx.mIsolate);
        }
        return Detail::lendToJs<false>(cx, value);
    }
};

} // namespace Alka
