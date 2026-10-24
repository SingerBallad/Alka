#pragma once

#include <atomic>
#include <memory>

namespace Alka {

class Trackable {
public:
    [[nodiscard]] auto alkaLiveness() const noexcept -> std::shared_ptr<const std::atomic<bool>> {
        return mAlkaAlive;
    }

protected:
    Trackable() = default;
    Trackable(const Trackable&) noexcept {}
    Trackable& operator=(const Trackable&) noexcept { return *this; }
    ~Trackable() { mAlkaAlive->store(false, std::memory_order_release); }

private:
    std::shared_ptr<std::atomic<bool>> mAlkaAlive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace Alka
