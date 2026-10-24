#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Runtime/Trackable.hpp>

#include <v8.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

// Lifetime tools for objects shared between C++ and JS.
//
//   * Alka::Trackable           -- base class: borrowed wrappers notice destruction
//   * Alka::invalidate          -- the same, explicitly, for types you cannot change
//   * Alka::pin                 -- a C++ share of a JS-owned object that keeps its
//                                  JS wrapper (identity, expandos, Retained values) alive
//   * Alka::updateExternalSize  -- re-report a JS-owned object's native memory
//   * Alka::setDestructionMode  -- run collected objects' destructors at safe points

namespace Alka {

template <class T> void invalidate(v8::Isolate* isolate, T* ptr) {
    static_assert(BoundClass<std::remove_cv_t<T>>,
                  "Alka: invalidate() takes an object of a bound class");
    if (ptr == nullptr) {
        return;
    }
    using U = std::remove_cv_t<T>;
    const auto at = Detail::locateNative<U>(const_cast<U*>(ptr));
    Detail::invalidateWrappers(isolate, at.mNode, at.mKey);
}

template <class T> void releaseKept(v8::Isolate* isolate, T* ptr) {
    static_assert(BoundClass<std::remove_cv_t<T>>,
                  "Alka: releaseKept() takes an object of a bound class");
    if (ptr == nullptr) {
        return;
    }
    using U = std::remove_cv_t<T>;
    const auto at = Detail::locateNative<U>(const_cast<U*>(ptr));
    Detail::releaseKeptValues(isolate, at.mNode, at.mKey);
}

inline void releaseKept(const CallInfo& call) {
    ConvertCtx cx{call.mIsolate, call.mContext};
    Detail::releaseKeptValues(cx, call.mThis);
}

template <class T> [[nodiscard]] auto pin(v8::Isolate* isolate, T* ptr) -> std::shared_ptr<T> {
    static_assert(BoundClass<std::remove_cv_t<T>>, "Alka: pin() takes an object of a bound class");
    if (ptr == nullptr) {
        return nullptr;
    }
    using U = std::remove_cv_t<T>;
    const auto at = Detail::locateNative<U>(const_cast<U*>(ptr));
    auto* holder = Detail::findOwningHolder(isolate, at.mNode, at.mKey);
    if (holder == nullptr) {
        return nullptr;
    }
    return std::shared_ptr<T>(Detail::pinnedOwner(*holder), ptr);
}

template <class T> void updateExternalSize(v8::Isolate* isolate, const T* ptr) {
    static_assert(BoundClass<std::remove_cv_t<T>>,
                  "Alka: updateExternalSize() takes an object of a bound class");
    if (ptr == nullptr) {
        return;
    }
    using U = std::remove_cv_t<T>;
    const auto at = Detail::locateNative<U>(const_cast<U*>(ptr));
    if (auto* holder = Detail::findOwningHolder(isolate, at.mNode, at.mKey); holder != nullptr) {
        const auto size = Detail::externalSizeOf<U>(*ptr);
        Detail::adjustExternalMemory(Detail::perIsolateData(isolate),
                                     size - holder->mExternalBytes);
        holder->mExternalBytes = size;
    }
}

[[nodiscard]] auto reportedExternalMemory(v8::Isolate* isolate) -> std::int64_t;

void setDestructionMode(v8::Isolate* isolate, DestructionMode mode);

auto runDeferredDestruction(v8::Isolate* isolate,
                            std::size_t max = (std::numeric_limits<std::size_t>::max)())
    -> std::size_t;

} // namespace Alka
