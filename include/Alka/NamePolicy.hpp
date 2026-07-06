#pragma once

#include <Alka/FixedString.hpp>

#include <cstddef>

namespace Alka {

enum class NameKind { Type, Method, StaticMethod, Property, Function, EnumValue, Module };

namespace Detail {

template<FixedString Raw>
[[nodiscard]] consteval bool hasMemberPrefix() {
    return Raw.size() >= 2 && Raw[0] == 'm' && Raw[1] >= 'A' && Raw[1] <= 'Z';
}

// "mHp" -> "hp", "mMaxSpeed" -> "maxSpeed"; anything without the mXxx shape
// passes through untouched.
template<FixedString Raw>
[[nodiscard]] consteval auto stripMemberPrefix() {
    if constexpr (hasMemberPrefix<Raw>()) {
        FixedString<Raw.size() - 1> out{};
        out.mData[0] = static_cast<char>(Raw[1] - 'A' + 'a');
        for (std::size_t i = 2; i < Raw.size(); ++i) {
            out.mData[i - 1] = Raw[i];
        }
        return out;
    } else {
        return Raw;
    }
}

}  // namespace Detail

struct DefaultNamePolicy {
    template<NameKind Kind, FixedString Raw>
    [[nodiscard]] static consteval auto transform() {
        if constexpr (Kind == NameKind::Property) {
            return Detail::stripMemberPrefix<Raw>();
        } else {
            return Raw;
        }
    }
};

template<class = void>
struct NamePolicy : DefaultNamePolicy {};

namespace Detail {

template<NameKind Kind, FixedString Raw>
inline constexpr auto transformedName = NamePolicy<>::transform<Kind, Raw>();

}  // namespace Detail

}  // namespace Alka
