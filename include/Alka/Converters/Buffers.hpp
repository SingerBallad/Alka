#pragma once

#include <Alka/Converter.hpp>
#include <Alka/Converters/Containers.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Alka {

namespace Detail {

[[nodiscard]] inline auto bytesToJs(const ConvertCtx& cx, const void* data, const std::size_t size)
    -> v8::Local<v8::Value> {
    const auto buffer = v8::ArrayBuffer::New(cx.mIsolate, size);
    if (size > 0) {
        std::memcpy(buffer->Data(), data, size);
    }
    return v8::Uint8Array::New(buffer, 0, size);
}

template<ByteLike B>
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

}  // namespace Detail

template<Detail::ByteLike B, class Alloc>
struct Converter<std::vector<B, Alloc>> {
    static auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value)
        -> Result<std::vector<B, Alloc>> {
        auto out = Detail::bytesFromJs<B>(cx, value);
        if (!out) {
            return std::unexpected(std::move(out).error());
        }
        return std::vector<B, Alloc>(out->begin(), out->end());
    }
    static v8::Local<v8::Value> toJs(const ConvertCtx& cx, const std::vector<B, Alloc>& value) {
        return Detail::bytesToJs(cx, value.data(), value.size());
    }
};

template<Detail::ByteLike B>
struct Converter<std::span<const B>> {
    static auto toJs(ConvertCtx& cx, std::span<const B> value) -> v8::Local<v8::Value> {
        return Detail::bytesToJs(cx, value.data(), value.size());
    }
};

}  // namespace Alka
