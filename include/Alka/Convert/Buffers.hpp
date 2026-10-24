#pragma once

#include <Alka/Convert/Containers.hpp>
#include <Alka/Convert/Converter.hpp>
#include <Alka/Convert/TypedArrays.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Alka {

namespace Detail {

[[nodiscard]] inline auto bytesToJs(ConvertCtx& cx, const void* data, const std::size_t size)
    -> v8::Local<v8::Value> {
    const auto buffer = v8::ArrayBuffer::New(cx.mIsolate, size);
    if (size > 0) {
        std::memcpy(buffer->Data(), data, size);
    }
    return v8::Uint8Array::New(buffer, 0, size);
}

template <ByteLike B>
[[nodiscard]] auto bytesFromJs(ConvertCtx&, v8::Local<v8::Value> value) -> Result<std::vector<B>> {
    if (!value.IsEmpty() && value->IsArrayBufferView()) {
        const auto view = value.As<v8::ArrayBufferView>();
        std::vector<B> out(view->ByteLength());
        view->CopyContents(out.data(), out.size());
        return out;
    }
    if (!value.IsEmpty() && value->IsArrayBuffer()) {
        const auto buffer = value.As<v8::ArrayBuffer>();
        std::vector<B> out(buffer->ByteLength());
        if (!out.empty()) {
            std::memcpy(out.data(), buffer->Data(), out.size());
        }
        return out;
    }
    return std::unexpected(
        ConvertError::typeMismatch("ArrayBuffer or TypedArray", describeJsValue(value)));
}

} // namespace Detail

template <Detail::ByteLike B, class Alloc> struct Converter<std::vector<B, Alloc>> {
    static auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value)
        -> Result<std::vector<B, Alloc>> {
        auto out = Detail::bytesFromJs<B>(cx, value);
        if (!out) {
            return std::unexpected(std::move(out).error());
        }
        return std::vector<B, Alloc>(out->begin(), out->end());
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const std::vector<B, Alloc>& value) {
        return Detail::bytesToJs(cx, value.data(), value.size());
    }
};

template <Detail::ByteLike B> struct Converter<std::span<const B>> {
    static auto toJs(ConvertCtx& cx, std::span<const B> value) -> v8::Local<v8::Value> {
        return Detail::bytesToJs(cx, value.data(), value.size());
    }
};

// void hash(Alka::BufferView bytes);                 // JS: hash(new Uint8Array(...))
// Alka::BufferView render() {                        // JS receives a Uint8Array
//     auto out = Alka::BufferView::allocate(isolate, 4096);  // over this memory,
//     fill(out.bytes());                             // no copy
//     return out;
// }
// Resizable and SharedArrayBuffer-backed data is rejected (its size or
// contents can change under C++).
class BufferView {
public:
    BufferView() = default;
    BufferView(std::shared_ptr<v8::BackingStore> store, const std::size_t offset,
               const std::size_t length)
        : mStore(std::move(store)), mOffset(offset), mLength(length) {}

    [[nodiscard]] static auto allocate(v8::Isolate* isolate, const std::size_t length)
        -> BufferView {
        std::shared_ptr store = v8::ArrayBuffer::NewBackingStore(isolate, length);
        return {std::move(store), 0, length};
    }

    [[nodiscard]] auto bytes() const noexcept -> std::span<std::byte> {
        if (!mStore || mLength == 0) {
            return {};
        }
        return {static_cast<std::byte*>(mStore->Data()) + mOffset, mLength};
    }
    [[nodiscard]] auto data() const noexcept -> std::byte* { return bytes().data(); }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return mLength; }
    [[nodiscard]] bool empty() const noexcept { return mLength == 0; }
    [[nodiscard]] auto backingStore() const noexcept -> const std::shared_ptr<v8::BackingStore>& {
        return mStore;
    }
    [[nodiscard]] auto offset() const noexcept -> std::size_t { return mOffset; }

private:
    std::shared_ptr<v8::BackingStore> mStore;
    std::size_t mOffset = 0;
    std::size_t mLength = 0;
};

// int checksum(Alka::SharedBufferView data);   // JS: checksum(new Uint8Array(sab))
class SharedBufferView : public BufferView {
public:
    using BufferView::BufferView;
    SharedBufferView() = default;

    [[nodiscard]] static auto allocate(v8::Isolate* isolate, const std::size_t length)
        -> SharedBufferView {
        std::shared_ptr store = v8::SharedArrayBuffer::NewBackingStore(isolate, length);
        return {std::move(store), 0, length};
    }
};

namespace Detail {

struct ViewMemory {
    std::shared_ptr<v8::BackingStore> mStore;
    std::size_t mOffset = 0;
    std::size_t mLength = 0;
};

[[nodiscard]] inline auto viewMemory(const v8::Local<v8::Value> value, const bool allowShared,
                                     const std::string_view as) -> Result<ViewMemory> {
    ViewMemory out;
    if (!value.IsEmpty() && value->IsArrayBufferView()) {
        const auto view = value.As<v8::ArrayBufferView>();
        out.mStore = view->Buffer()->GetBackingStore();
        out.mOffset = view->ByteOffset();
        out.mLength = view->ByteLength();
    } else if (!value.IsEmpty() && value->IsArrayBuffer()) {
        const auto buffer = value.As<v8::ArrayBuffer>();
        out.mStore = buffer->GetBackingStore();
        out.mLength = buffer->ByteLength();
    } else if (allowShared && !value.IsEmpty() && value->IsSharedArrayBuffer()) {
        const auto buffer = value.As<v8::SharedArrayBuffer>();
        out.mStore = buffer->GetBackingStore();
        out.mLength = buffer->ByteLength();
    } else {
        return std::unexpected(
            ConvertError::typeMismatch(allowShared ? "ArrayBuffer, SharedArrayBuffer or TypedArray"
                                                   : "ArrayBuffer or TypedArray",
                                       describeJsValue(value)));
    }
    if (out.mStore->IsShared() && !allowShared) {
        return std::unexpected(
            ConvertError{"SharedArrayBuffer data is not accepted as a " + std::string(as)});
    }
    if (out.mStore->IsResizableByUserJavaScript() && !out.mStore->IsShared()) {
        return std::unexpected(
            ConvertError{"a resizable ArrayBuffer is not accepted as a " + std::string(as)});
    }
    return out;
}

[[nodiscard]] inline auto viewToJs(ConvertCtx& cx, const BufferView& value)
    -> v8::Local<v8::Value> {
    const auto& store = value.backingStore();
    if (!store) {
        return v8::Uint8Array::New(v8::ArrayBuffer::New(cx.mIsolate, 0), 0, 0);
    }
    if (store->IsShared()) {
        return v8::Uint8Array::New(v8::SharedArrayBuffer::New(cx.mIsolate, store), value.offset(),
                                   value.size());
    }
    return v8::Uint8Array::New(v8::ArrayBuffer::New(cx.mIsolate, store), value.offset(),
                               value.size());
}

} // namespace Detail

template <> struct Converter<BufferView> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<BufferView> {
        auto memory = Detail::viewMemory(value, false, "BufferView");
        if (!memory) {
            return std::unexpected(std::move(memory).error());
        }
        return BufferView(std::move(memory->mStore), memory->mOffset, memory->mLength);
    }
    static auto toJs(ConvertCtx& cx, const BufferView& value) -> v8::Local<v8::Value> {
        return Detail::viewToJs(cx, value);
    }
};

// void normalize(Alka::TypedView<float> v);          // JS: normalize(new Float32Array(...))
// Alka::TypedView<float> mesh() {                    // JS receives a Float32Array
//     auto out = Alka::TypedView<float>::allocate(isolate, 3 * n);   // over this memory
//     ...
template <Detail::TypedElement T> class TypedView {
public:
    TypedView() = default;
    TypedView(std::shared_ptr<v8::BackingStore> store, const std::size_t byteOffset,
              const std::size_t length)
        : mStore(std::move(store)), mByteOffset(byteOffset), mLength(length) {}

    [[nodiscard]] static auto allocate(v8::Isolate* isolate, const std::size_t length)
        -> TypedView {
        std::shared_ptr store = v8::ArrayBuffer::NewBackingStore(isolate, length * sizeof(T));
        return TypedView(std::move(store), 0, length);
    }

    [[nodiscard]] auto span() const noexcept -> std::span<T> {
        if (!mStore || mLength == 0) {
            return {};
        }
        return {reinterpret_cast<T*>(static_cast<std::byte*>(mStore->Data()) + mByteOffset),
                mLength};
    }
    [[nodiscard]] auto data() const noexcept -> T* { return span().data(); }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return mLength; }
    [[nodiscard]] bool empty() const noexcept { return mLength == 0; }
    [[nodiscard]] auto operator[](const std::size_t i) const noexcept -> T& { return span()[i]; }
    [[nodiscard]] auto begin() const noexcept { return span().begin(); }
    [[nodiscard]] auto end() const noexcept { return span().end(); }
    [[nodiscard]] auto backingStore() const noexcept -> const std::shared_ptr<v8::BackingStore>& {
        return mStore;
    }
    [[nodiscard]] auto byteOffset() const noexcept -> std::size_t { return mByteOffset; }

private:
    std::shared_ptr<v8::BackingStore> mStore;
    std::size_t mByteOffset = 0;
    std::size_t mLength = 0;
};

template <Detail::TypedElement T> class SharedTypedView : public TypedView<T> {
public:
    using TypedView<T>::TypedView;
    SharedTypedView() = default;

    [[nodiscard]] static auto allocate(v8::Isolate* isolate, const std::size_t length)
        -> SharedTypedView {
        std::shared_ptr store = v8::SharedArrayBuffer::NewBackingStore(isolate, length * sizeof(T));
        return SharedTypedView(std::move(store), 0, length);
    }
};

namespace Detail {

template <TypedElement T>
[[nodiscard]] auto typedViewMemory(const v8::Local<v8::Value> value, const bool allowShared,
                                   const std::string_view as) -> Result<ViewMemory> {
    constexpr auto kind = typedKindOf<T>();
    if (typedKindOfValue(value) != kind) {
        return std::unexpected(
            ConvertError::typeMismatch(typedArrayName(kind), describeJsValue(value)));
    }
    const auto array = value.As<v8::TypedArray>();
    ViewMemory out;
    out.mStore = array->Buffer()->GetBackingStore();
    out.mOffset = array->ByteOffset();
    out.mLength = array->Length();
    if (out.mStore->IsShared() && !allowShared) {
        return std::unexpected(
            ConvertError{"SharedArrayBuffer data is not accepted as a " + std::string(as)});
    }
    if (out.mStore->IsResizableByUserJavaScript() && !out.mStore->IsShared()) {
        return std::unexpected(
            ConvertError{"a resizable ArrayBuffer is not accepted as a " + std::string(as)});
    }
    return out;
}

template <TypedElement T>
[[nodiscard]] auto typedViewToJs(ConvertCtx& cx, const TypedView<T>& value)
    -> v8::Local<v8::Value> {
    const auto& store = value.backingStore();
    if (!store) {
        return newTypedArray<T>(v8::ArrayBuffer::New(cx.mIsolate, 0), 0, 0);
    }
    if (store->IsShared()) {
        return newTypedArray<T>(v8::SharedArrayBuffer::New(cx.mIsolate, store), value.byteOffset(),
                                value.size());
    }
    return newTypedArray<T>(v8::ArrayBuffer::New(cx.mIsolate, store), value.byteOffset(),
                            value.size());
}

} // namespace Detail

template <Detail::TypedElement T> struct Converter<TypedView<T>> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<TypedView<T>> {
        auto memory = Detail::typedViewMemory<T>(value, false, "TypedView");
        if (!memory) {
            return std::unexpected(std::move(memory).error());
        }
        return TypedView<T>(std::move(memory->mStore), memory->mOffset, memory->mLength);
    }
    static auto toJs(ConvertCtx& cx, const TypedView<T>& value) -> v8::Local<v8::Value> {
        return Detail::typedViewToJs(cx, value);
    }
};

template <Detail::TypedElement T> struct Converter<SharedTypedView<T>> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value)
        -> Result<SharedTypedView<T>> {
        auto memory = Detail::typedViewMemory<T>(value, true, "SharedTypedView");
        if (!memory) {
            return std::unexpected(std::move(memory).error());
        }
        return SharedTypedView<T>(std::move(memory->mStore), memory->mOffset, memory->mLength);
    }
    static auto toJs(ConvertCtx& cx, const SharedTypedView<T>& value) -> v8::Local<v8::Value> {
        return Detail::typedViewToJs<T>(cx, value);
    }
};

template <> struct Converter<SharedBufferView> {
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<SharedBufferView> {
        auto memory = Detail::viewMemory(value, true, "SharedBufferView");
        if (!memory) {
            return std::unexpected(std::move(memory).error());
        }
        return SharedBufferView(std::move(memory->mStore), memory->mOffset, memory->mLength);
    }
    static auto toJs(ConvertCtx& cx, const SharedBufferView& value) -> v8::Local<v8::Value> {
        return Detail::viewToJs(cx, value);
    }
};

} // namespace Alka
