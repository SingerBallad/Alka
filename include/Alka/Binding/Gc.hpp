#pragma once

#include <Alka/Config.hpp>

// Bound classes on V8's C++ heap (cppgc, "Oilpan"): derive from
// Alka::GcObject<T>, and the object lives on the isolate's CppHeap, traced
// together with JS. JS and C++ references between such objects form one
// graph -- cycles through either side are collected -- and their wrappers
// need no weak callbacks or ownership bookkeeping: the wrapper keeps the
// object alive, and the object (while reachable) its one wrapper.
//
//   class Node : public Alka::GcObject<Node> {
//   public:
//       void Trace(cppgc::Visitor* visitor) const override {
//           GcObject::Trace(visitor);  // (its wrapper)
//           visitor->Trace(mNext);
//       }
//       cppgc::Member<Node> mNext;    // JS sees a Node or null
//   };
//   template<> struct Alka::Bind<Node> : Alka::Class<Node> {
//       static constexpr auto members = Alka::members(
//           Alka::ctor<>(), Alka::prop<"next", &Node::mNext>());
//   };
//
// The isolate needs a CppHeap (IsolateOptions::mCppHeap, a CreateParams
// cpp_heap, or Node's own). C++ creates objects with Alka::makeGc<T>(...)
// and holds them across calls in cppgc::Persistent<T>; GC classes travel as
// T* / T&, cppgc::Member / WeakMember / Persistent / WeakPersistent (JS sees
// the object or null) -- never by value or in a shared_ptr. V8 13+ (on
// Windows with a V8 DLL: USING_V8_SHARED).

#if ALKA_HAS_CPPGC

#include <Alka/Convert/Converter.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Detail/TypeNode.hpp>

#include <cppgc/allocation.h>
#include <cppgc/garbage-collected.h>
#include <cppgc/member.h>
#include <cppgc/persistent.h>
#include <cppgc/trace-trait.h>
#include <cppgc/visitor.h>
#include <v8-cppgc.h>
#include <v8-traced-handle.h>
#include <v8.h>

#include <string>
#include <utility>

#if defined(_MSC_VER) && !defined(__clang__) && defined(USING_V8_SHARED)
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "/alternatename:__imp_?Current@SourceLocation@v8@@SA?AV12@XZ=alka_SourceLocation_Current")
#endif

namespace Alka {

namespace Detail {

struct GcWrapperSlot {
    v8::TracedReference<v8::Object> mAlkaWrapper;
};

template <class T, class O = v8::Object> struct GcBase {
    using Type = cppgc::GarbageCollected<T>;
};
template <class T, class O>
    requires requires { typename O::Wrappable; }
struct GcBase<T, O> {
    using Type = typename O::Wrappable;
};

} // namespace Detail

template <class T> class GcObject : public Detail::GcBase<T>::Type, public Detail::GcWrapperSlot {
public:
    virtual ~GcObject() = default;
    virtual void Trace(cppgc::Visitor* visitor) const { visitor->Trace(mAlkaWrapper); }
};

namespace Detail {

template <class T, class... Args> auto makeGcIn(v8::Isolate* isolate, Args&&... args) -> T* {
    auto* const heap = isolate != nullptr ? isolate->GetCppHeap() : nullptr;
    if (heap == nullptr) {
        throw JsException(std::string("Alka: a ") + std::string(kTypeNode<T>.mName) +
                          " lives on V8's C++ heap, and this isolate has none (create it with "
                          "IsolateOptions::mCppHeap, or CreateParams::cpp_heap)");
    }
    return cppgc::MakeGarbageCollected<T>(heap->GetAllocationHandle(), std::forward<Args>(args)...);
}

template <class Tag = v8::CppHeapPointerTag>
[[nodiscard]] consteval auto cppHeapTag() -> v8::CppHeapPointerTag {
#ifdef ALKA_CPPHEAP_TAG
    return static_cast<Tag>(ALKA_CPPHEAP_TAG);
#else
    if constexpr (requires { Tag::kLastEmbedderWrappableTag; }) {
        return Tag::kLastEmbedderWrappableTag;
    } else {
        return Tag::kDefaultTag;
    }
#endif
}

template <class Wrappable>
void wrapIn(v8::Isolate* isolate, const v8::Local<v8::Object> wrapper, Wrappable* wrappable) {
    v8::Object::Wrap<cppHeapTag()>(isolate, wrapper, wrappable);
}

template <class U>
void attachGc(v8::Isolate* isolate, const v8::Local<v8::Object> wrapper, const TypeNode* node,
              void* ptr, U* object) {
    writeGcInternals(wrapper, node, ptr);
    using Base = typename GcBase<U>::Type;
    if constexpr (std::is_same_v<Base, cppgc::GarbageCollected<U>>) {
        // (where the heap object starts)
        const void* const start =
            cppgc::TraceTrait<U>::GetTraceDescriptor(object).base_object_payload;
        wrapIn(isolate, wrapper, const_cast<void*>(start));
    } else {
        wrapIn(isolate, wrapper, static_cast<Base*>(object));
    }
    static_cast<GcWrapperSlot&>(*object).mAlkaWrapper.Reset(isolate, wrapper);
}

template <class T> auto wrapGc(ConvertCtx& cx, T* ptr) -> v8::Local<v8::Value> {
    if (ptr == nullptr) {
        return v8::Null(cx.mIsolate);
    }
    using U = std::remove_cv_t<T>;
    auto* const object = const_cast<U*>(ptr);
    GcWrapperSlot& slot = *object;
    if (!slot.mAlkaWrapper.IsEmpty()) {
        return slot.mAlkaWrapper.Get(cx.mIsolate);
    }
    const auto at = locateNative<U>(object);
    void* adjusted = at.mPtr;
    const TypeNode* node = at.mNode;
    const auto wrapper = instantiateWrapper(cx, node, adjusted);
    attachGc(cx.mIsolate, wrapper, node, adjusted, object);
    return wrapper;
}

template <class T>
void attachGcConstructed(ConvertCtx& cx, const v8::Local<v8::Object> self, T* object) {
    attachGc(cx.mIsolate, self, &kTypeNode<T>, object, object);
}

} // namespace Detail

template <class T, class... Args>
    requires isGcClass<T>
[[nodiscard]] auto makeGc(Args&&... args) -> T* {
    return Detail::makeGcIn<T>(v8::Isolate::TryGetCurrent(), std::forward<Args>(args)...);
}

template <class U>
    requires isGcClass<U>
struct Converter<cppgc::Member<U>> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<cppgc::Member<U>> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return cppgc::Member<U>();
        }
        auto unwrapped = Detail::unwrapObject<U>(value);
        if (!unwrapped) {
            return std::unexpected(std::move(unwrapped).error());
        }
        return cppgc::Member<U>(unwrapped->mPtr);
    }
    static auto toJs(ConvertCtx& cx, const cppgc::Member<U>& value) -> v8::Local<v8::Value> {
        return Detail::wrapGc(cx, value.Get());
    }
};

template <class U>
    requires isGcClass<U>
struct Converter<cppgc::WeakMember<U>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<cppgc::WeakMember<U>> {
        auto strong = Converter<cppgc::Member<U>>::fromJs(cx, value);
        if (!strong) {
            return std::unexpected(std::move(strong).error());
        }
        return cppgc::WeakMember<U>(strong->Get());
    }
    static auto toJs(ConvertCtx& cx, const cppgc::WeakMember<U>& value) -> v8::Local<v8::Value> {
        return Detail::wrapGc(cx, value.Get());
    }
};

template <class U>
    requires isGcClass<U>
struct Converter<cppgc::Persistent<U>> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value)
        -> Result<cppgc::Persistent<U>> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return cppgc::Persistent<U>();
        }
        auto unwrapped = Detail::unwrapObject<U>(value);
        if (!unwrapped) {
            return std::unexpected(std::move(unwrapped).error());
        }
        return cppgc::Persistent<U>(unwrapped->mPtr);
    }
    static auto toJs(ConvertCtx& cx, const cppgc::Persistent<U>& value) -> v8::Local<v8::Value> {
        return Detail::wrapGc(cx, value.Get());
    }
};

template <class U>
    requires isGcClass<U>
struct Converter<cppgc::WeakPersistent<U>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<cppgc::WeakPersistent<U>> {
        auto strong = Converter<cppgc::Persistent<U>>::fromJs(cx, value);
        if (!strong) {
            return std::unexpected(std::move(strong).error());
        }
        return cppgc::WeakPersistent<U>(strong->Get());
    }
    static auto toJs(ConvertCtx& cx, const cppgc::WeakPersistent<U>& value)
        -> v8::Local<v8::Value> {
        return Detail::wrapGc(cx, value.Get()); // (null once collected)
    }
};

} // namespace Alka

#endif // ALKA_HAS_CPPGC
