#pragma once

#include <memory>
#include <utility>

namespace Alka::Detail {

struct Holder {
    void* mPtr = nullptr;

    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
    virtual ~Holder() = default;

    [[nodiscard]] virtual auto sharedOwner() const noexcept -> std::shared_ptr<void> { return nullptr; }

protected:
    explicit Holder(void* ptr) noexcept : mPtr(ptr) {}
};

template<class T>
struct OwnedHolder final : Holder {
    std::unique_ptr<T> mOwned;

    explicit OwnedHolder(std::unique_ptr<T> owned) noexcept
        : Holder(owned.get()), mOwned(std::move(owned)) {}
};

template<class T>
struct SharedHolder final : Holder {
    std::shared_ptr<T> mShared;

    explicit SharedHolder(std::shared_ptr<T> shared) noexcept
        : Holder(shared.get()), mShared(std::move(shared)) {}

    [[nodiscard]] auto sharedOwner() const noexcept -> std::shared_ptr<void> override {
        return mShared;
    }
};

struct RefHolder final : Holder {
    explicit RefHolder(void* ptr) noexcept : Holder(ptr) {}
};

}  // namespace Alka::Detail
