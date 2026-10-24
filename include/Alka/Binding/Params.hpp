#pragma once

#include <v8.h>

#include <type_traits>
#include <vector>

namespace Alka {

struct ConvertCtx;

template <class T> struct Rest : std::vector<T> {
    using std::vector<T>::vector;
};

struct CallInfo {
    v8::Isolate* mIsolate = nullptr;
    v8::Local<v8::Context> mContext;
    v8::Local<v8::Object> mThis;     // the receiver (the wrapper for methods)
    v8::Local<v8::Value> mNewTarget; // undefined unless constructing
    const v8::FunctionCallbackInfo<v8::Value>* mRaw = nullptr; // escape hatch
};

namespace Detail {

template <class X> inline constexpr bool isRestImpl = false;
template <class T> inline constexpr bool isRestImpl<Rest<T>> = true;

template <class X>
inline constexpr bool isInjectedImpl =
    std::is_same_v<X, CallInfo> || std::is_same_v<X, v8::Isolate*> ||
    std::is_same_v<X, ConvertCtx> || std::is_same_v<X, v8::Local<v8::Context>>;

} // namespace Detail

template <class X> inline constexpr bool isRest = Detail::isRestImpl<std::remove_cvref_t<X>>;

template <class X>
inline constexpr bool isInjected = Detail::isInjectedImpl<std::remove_cvref_t<X>>;

} // namespace Alka
