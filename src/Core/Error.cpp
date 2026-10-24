#include <Alka/Core/Error.hpp>

#include <format>

namespace Alka {

auto ConvertError::typeMismatch(std::string_view expected, std::string_view got) -> ConvertError {
    return {std::format("expected {}, got {}", expected, got)};
}

auto ConvertError::outOfRange(std::string_view expected, std::string_view got) -> ConvertError {
    return {std::format("{} cannot represent {}", expected, got)};
}

auto ConvertError::forArgument(ConvertError inner, std::size_t index, std::string_view argName,
                               std::string_view functionName) -> ConvertError {
    if (argName.empty()) {
        return {std::format("arg {} of {}: {}", index + 1, functionName, inner.mMessage),
                std::move(inner.mJsException)};
    }
    return {std::format("arg {} ({}) of {}: {}", index + 1, argName, functionName, inner.mMessage),
            std::move(inner.mJsException)};
}

auto ConvertError::forElement(ConvertError inner, std::size_t index) -> ConvertError {
    return {std::format("element {}: {}", index, inner.mMessage), std::move(inner.mJsException)};
}

auto ConvertError::forKey(ConvertError inner, std::string_view key) -> ConvertError {
    return {std::format("key '{}': {}", key, inner.mMessage), std::move(inner.mJsException)};
}

} // namespace Alka
