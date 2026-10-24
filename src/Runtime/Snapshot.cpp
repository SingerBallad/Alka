#include <Alka/Runtime/Snapshot.hpp>

#include <new>

namespace Alka::snapshot {

namespace Detail {

namespace {

constexpr char kDetachedMarker = 0x41;

} // namespace

auto serializeInternalField(const v8::Local<v8::Object> holder, const int index, void*)
    -> v8::StartupData {
    if (index != kNodeField && index != kHolderField) {
        return {.data = nullptr, .raw_size = 0};
    }
    if (const auto* node = readNode(holder); node == nullptr || !isKnownTypeNode(node)) {
        return {.data = nullptr, .raw_size = 0};
    }
    auto* payload = new char[1]{kDetachedMarker};
    return {.data = payload, .raw_size = 1};
}

void deserializeInternalField(const v8::Local<v8::Object> holder, const int index,
                              const v8::StartupData payload, void*) {
    if (payload.raw_size == 1 && payload.data != nullptr && payload.data[0] == kDetachedMarker) {
        clearInternal(holder, index);
    }
}

} // namespace Detail

void prepare(v8::Isolate* isolate) {
    disposeIsolate(isolate);
}

} // namespace Alka::snapshot
