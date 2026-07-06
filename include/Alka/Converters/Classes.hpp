#pragma once

#include <Alka/Converter.hpp>
#include <Alka/Invoke.hpp>

#include <memory>
#include <type_traits>

namespace Alka {

template<BoundClass T>
    requires std::copy_constructible<T>
struct Converter<T> {
    static auto fromJs(ConvertCtx&, v8::Local<v8::Value> value) -> Result<T> {
        auto unwrapped = Detail::unwrapObject<T>(value);
        if (!unwrapped) {
            return std::unexpected(std::move(unwrapped).error());
        }
        return T(**unwrapped);
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const T& value) {
        return Detail::wrapOwned(cx, std::make_unique<T>(value));
    }
};

template<BoundClass T>
struct Converter<std::shared_ptr<T>> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value)
        -> Result<std::shared_ptr<T>> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return std::shared_ptr<T>{};
        }
        auto unwrapped = Detail::unwrapObjectWithHolder<T>(value);
        if (!unwrapped) {
            return std::unexpected(std::move(unwrapped).error());
        }
        auto [ptr, holder] = *unwrapped;
        std::shared_ptr<void> owner = holder->sharedOwner();
        if (!owner) {
            return std::unexpected(ConvertError{
                "this " + std::string(Detail::kTypeNode<T>.mName) +
                " is not shared_ptr-managed (it was created by JS or borrowed from C++)"});
        }
        return std::shared_ptr<T>(std::move(owner), ptr);  // aliasing ctor
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const std::shared_ptr<T>& value) {
        if (!value) {
            return v8::Null(cx.mIsolate);
        }
        return Detail::wrapShared(cx, value);
    }
};

template<class T>
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
        return *unwrapped;
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, T* const& value) {
        if (value == nullptr) {
            return v8::Null(cx.mIsolate);
        }
        return Detail::wrapBorrowed(cx, value);
    }
};

}  // namespace Alka
