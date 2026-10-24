#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace Alka::Detail {

struct WrapperEntry;
struct TypeNode;
struct Holder;

enum class Ownership : std::uint8_t {
    Borrowed, // C++ owns it; the wrapper only references it (T* / T& returns)
    Owned,    // the wrapper holds (shared) ownership: JS-created, by-value,
              // unique_ptr and shared_ptr returns all end up here
    Weak,     // weak_ptr return: usable only while C++ keeps it alive
};

using PartResolver = void* (*)(const Holder& view, void* start, std::string& error);

struct Holder {
    void* mPtr = nullptr; // the object
    Ownership mOwnership = Ownership::Borrowed;
    std::shared_ptr<void> mOwner;                      // Owned
    std::weak_ptr<void> mWeak;                         // Weak
    std::shared_ptr<const std::atomic<bool>> mTracked; // Borrowed Trackable: liveness flag
    std::weak_ptr<void> mPin;                          // live while C++ holds a pinned share
    const Holder* mParent = nullptr;                   // a view into another wrapped object
    WrapperEntry* mEntry = nullptr; // the wrapper bookkeeping, if any reported to V8
    std::int64_t mExternalBytes = 0;

    PartResolver mResolve = nullptr;
    const TypeNode* mPartNode = nullptr; // the part's node
    std::ptrdiff_t mStartDelta = 0;      // owner's current object
    void* mGcOwner = nullptr;            // a C++-heap owner
    std::size_t mPartIndex = 0;          // an element's index
    std::shared_ptr<void> mPartArgs;     // an accessor's arguments

    [[nodiscard]] static auto borrowed(void* ptr) -> std::unique_ptr<Holder> {
        auto h = std::make_unique<Holder>();
        h->mPtr = ptr;
        return h;
    }
    [[nodiscard]] static auto owned(std::shared_ptr<void> owner, void* ptr)
        -> std::unique_ptr<Holder> {
        auto h = std::make_unique<Holder>();
        h->mPtr = ptr;
        h->mOwnership = Ownership::Owned;
        h->mOwner = std::move(owner);
        return h;
    }
    [[nodiscard]] static auto weak(std::weak_ptr<void> owner, void* ptr)
        -> std::unique_ptr<Holder> {
        auto h = std::make_unique<Holder>();
        h->mPtr = ptr;
        h->mOwnership = Ownership::Weak;
        h->mWeak = std::move(owner);
        return h;
    }

    [[nodiscard]] bool borrowed() const noexcept { return mOwnership == Ownership::Borrowed; }
    [[nodiscard]] bool isPath() const noexcept { return mResolve != nullptr; }

    [[nodiscard]] bool alive() const noexcept {
        if (mPtr == nullptr) {
            return false;
        }
        switch (mOwnership) {
        case Ownership::Weak:
            return !mWeak.expired();
        case Ownership::Borrowed:
            return (mTracked == nullptr || mTracked->load(std::memory_order_acquire)) &&
                   (mParent == nullptr || mParent->alive());
        case Ownership::Owned:
            return true;
        }
        return false;
    }

    [[nodiscard]] auto resolve(std::string& error) const -> void* {
        if (!alive()) {
            return nullptr;
        }
        if (mResolve == nullptr) {
            return mPtr;
        }
        void* const owner = mParent != nullptr ? mParent->resolve(error) : mGcOwner;
        if (owner == nullptr) {
            return nullptr;
        }
        return mResolve(*this, static_cast<char*>(owner) + mStartDelta, error);
    }

    [[nodiscard]] auto lockForCall() const -> std::shared_ptr<void> {
        if (mParent != nullptr) {
            return mParent->lockForCall();
        }
        return mOwnership == Ownership::Weak ? mWeak.lock() : nullptr;
    }

    [[nodiscard]] auto shareOwnership() const -> std::shared_ptr<void> {
        if (mResolve != nullptr) {
            return nullptr;
        }
        if (mParent != nullptr) {
            auto owner = mParent->shareOwnership();
            return owner ? std::shared_ptr<void>(std::move(owner), mPtr) : nullptr;
        }
        switch (mOwnership) {
        case Ownership::Owned:
            return mOwner;
        case Ownership::Weak:
            return mWeak.lock();
        case Ownership::Borrowed:
            return nullptr;
        }
        return nullptr;
    }
};

} // namespace Alka::Detail
