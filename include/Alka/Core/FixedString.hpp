#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace Alka {

template <std::size_t N> struct FixedString {
    char mData[N + 1]{};

    constexpr FixedString() = default;

    constexpr FixedString(const char (&str)[N + 1]) { std::copy_n(str, N + 1, mData); }

    static constexpr FixedString from(std::string_view text) {
        FixedString out{};
        std::copy_n(text.data(), (std::min)(N, text.size()), out.mData);
        return out;
    }

    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view { return {mData, N}; }
    [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return N; }
    [[nodiscard]] constexpr bool empty() const noexcept { return N == 0; }
    [[nodiscard]] constexpr auto c_str() const noexcept -> const char* { return mData; }
    constexpr operator std::string_view() const noexcept { return view(); }
    [[nodiscard]] constexpr char operator[](std::size_t i) const noexcept { return mData[i]; }

    template <std::size_t M>
    [[nodiscard]] constexpr bool operator==(const FixedString<M>& other) const noexcept {
        return view() == other.view();
    }
    [[nodiscard]] constexpr bool operator==(const std::string_view other) const noexcept {
        return view() == other;
    }

    template <std::size_t M>
    [[nodiscard]] constexpr FixedString<N + M> operator+(const FixedString<M>& other) const {
        FixedString<N + M> out{};
        std::copy_n(mData, N, out.mData);
        std::copy_n(other.mData, M + 1, out.mData + N);
        return out;
    }
};

template <std::size_t M> FixedString(const char (&)[M]) -> FixedString<M - 1>;

} // namespace Alka
