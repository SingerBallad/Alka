#pragma once

#include <v8.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace Alka::Detail {

template <class O = v8::Object>
[[nodiscard]] auto prototypeOf(const v8::Local<O> object) -> v8::Local<v8::Value> {
    if constexpr (requires { object->GetPrototypeV2(); }) {
        return object->GetPrototypeV2();
    } else {
        return object->GetPrototype();
    }
}

template <class O = v8::Object>
[[nodiscard]] auto setPrototypeOf(const v8::Local<v8::Context> context, const v8::Local<O> object,
                                  const v8::Local<v8::Value> prototype) -> v8::Maybe<bool> {
    if constexpr (requires { object->SetPrototypeV2(context, prototype); }) {
        return object->SetPrototypeV2(context, prototype);
    } else {
        return object->SetPrototype(context, prototype);
    }
}

using EmbedderTag = std::uint16_t;

template <class O = v8::Object>
[[nodiscard]] auto internalPointer(const v8::Local<O> object, const int index,
                                   const EmbedderTag tag) -> void* {
    if constexpr (requires { object->GetAlignedPointerFromInternalField(index, tag); }) {
        return object->GetAlignedPointerFromInternalField(index, tag);
    } else {
        return object->GetAlignedPointerFromInternalField(index);
    }
}

template <class O = v8::Object>
void setInternalPointer(const v8::Local<O> object, const int index, void* value,
                        const EmbedderTag tag) {
    if constexpr (requires { object->SetAlignedPointerInInternalField(index, value, tag); }) {
        object->SetAlignedPointerInInternalField(index, value, tag);
    } else {
        object->SetAlignedPointerInInternalField(index, value);
    }
}

template <class C = v8::Context>
[[nodiscard]] auto contextPointer(v8::Isolate* isolate, const v8::Local<C> context, const int index,
                                  const EmbedderTag tag) -> void* {
    if constexpr (requires { context->GetAlignedPointerFromEmbedderData(isolate, index, tag); }) {
        return context->GetAlignedPointerFromEmbedderData(isolate, index, tag);
    } else if constexpr (requires { context->GetAlignedPointerFromEmbedderData(isolate, index); }) {
        return context->GetAlignedPointerFromEmbedderData(isolate, index);
    } else {
        return context->GetAlignedPointerFromEmbedderData(index);
    }
}

template <class C = v8::Context>
[[nodiscard]] auto contextSlotValue(const v8::Local<C> context, const int index)
    -> v8::Local<v8::Value> {
    if constexpr (requires { context->GetEmbedderDataV2(index); }) {
        const auto data = context->GetEmbedderDataV2(index);
        return !data.IsEmpty() && data->IsValue() ? data.template As<v8::Value>()
                                                  : v8::Local<v8::Value>();
    } else {
        return context->GetEmbedderData(index);
    }
}

template <class C = v8::Context>
void setContextPointer(const v8::Local<C> context, const int index, void* value,
                       const EmbedderTag tag) {
    if constexpr (requires { context->SetAlignedPointerInEmbedderData(index, value, tag); }) {
        context->SetAlignedPointerInEmbedderData(index, value, tag);
    } else {
        context->SetAlignedPointerInEmbedderData(index, value);
    }
}

using ExternalTag = std::uint16_t;

#if defined(ALKA_EXTERNAL_TAG)
inline constexpr ExternalTag kExternalTagBase = ALKA_EXTERNAL_TAG;
#elif defined(V8_EXTERNAL_POINTER_TAG_COUNT)
inline constexpr ExternalTag kExternalTagBase = V8_EXTERNAL_POINTER_TAG_COUNT - 4;
#else
inline constexpr ExternalTag kExternalTagBase = 0; // (no tags)
#endif
#if defined(V8_EXTERNAL_POINTER_TAG_COUNT)
static_assert(kExternalTagBase + 2 < V8_EXTERNAL_POINTER_TAG_COUNT - 1,
              "Alka: ALKA_EXTERNAL_TAG + 2 must stay below V8_EXTERNAL_POINTER_TAG_COUNT - 1");
#endif

inline constexpr ExternalTag kNativeFunctionTag = kExternalTagBase;
inline constexpr ExternalTag kGeneratorTag = kExternalTagBase + 1;
inline constexpr ExternalTag kAwaiterTag = kExternalTagBase + 2;

template <class I = v8::Isolate>
[[nodiscard]] auto newExternal(I* isolate, void* value, const ExternalTag tag)
    -> v8::Local<v8::External> {
    if constexpr (requires { v8::External::New(isolate, value, tag); }) {
        return v8::External::New(isolate, value, tag);
    } else {
        return v8::External::New(isolate, value);
    }
}

template <class E = v8::External>
[[nodiscard]] auto externalValue(const v8::Local<E> external, const ExternalTag tag) -> void* {
    if constexpr (requires { external->Value(tag); }) {
        return external->Value(tag);
    } else {
        return external->Value();
    }
}

template <class O = v8::Object>
[[nodiscard]] auto creationContextOf(v8::Isolate* isolate, const v8::Local<O> object)
    -> v8::MaybeLocal<v8::Context> {
    if constexpr (requires { object->GetCreationContext(isolate); }) {
        return object->GetCreationContext(isolate);
    } else {
        return object->GetCreationContext();
    }
}

using IndexedInterceptorGetter = v8::Intercepted (*)(std::uint32_t,
                                                     const v8::PropertyCallbackInfo<v8::Value>&);
using IndexedBooleanSetter = v8::Intercepted (*)(std::uint32_t, v8::Local<v8::Value>,
                                                 const v8::PropertyCallbackInfo<v8::Boolean>&);
using SetterInfo = v8::PropertyCallbackInfo<
    std::conditional_t<std::is_constructible_v<v8::IndexedPropertyHandlerConfiguration,
                                               IndexedInterceptorGetter, IndexedBooleanSetter>,
                       v8::Boolean, void>>;

template <class Info> [[nodiscard]] auto holderOf(const Info& info) -> v8::Local<v8::Object> {
    if constexpr (requires { info.HolderV2(); }) {
        return info.HolderV2();
    } else {
        return info.Holder();
    }
}

template <class S = v8::String, class Visit>
decltype(auto) visitString(v8::Isolate* isolate, const v8::Local<S> string, Visit&& visit) {
    if constexpr (requires { typename S::ValueView; }) {
        const typename S::ValueView view(isolate, string);
        const auto length = static_cast<std::size_t>(view.length());
        if (view.is_one_byte()) {
            return std::forward<Visit>(visit)(view.data8(), length);
        }
        return std::forward<Visit>(visit)(view.data16(), length);
    } else {
        constexpr std::size_t kStack = 128;
        const auto length = static_cast<std::size_t>(string->Length());
        if (string->IsOneByte()) {
            std::uint8_t stack[kStack];
            std::vector<std::uint8_t> heap(length > kStack ? length : 0);
            auto* const buffer = length > kStack ? heap.data() : stack;
            string->WriteOneByte(isolate, buffer, 0, static_cast<int>(length),
                                 S::NO_NULL_TERMINATION);
            return std::forward<Visit>(visit)(static_cast<const std::uint8_t*>(buffer), length);
        }
        std::uint16_t stack[kStack];
        std::vector<std::uint16_t> heap(length > kStack ? length : 0);
        auto* const buffer = length > kStack ? heap.data() : stack;
        string->Write(isolate, buffer, 0, static_cast<int>(length), S::NO_NULL_TERMINATION);
        return std::forward<Visit>(visit)(static_cast<const std::uint16_t*>(buffer), length);
    }
}

} // namespace Alka::Detail
