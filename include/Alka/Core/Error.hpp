#pragma once

#include <exception>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Alka {

namespace Detail {
struct ValueState;
} // namespace Detail

struct ConvertError {
    std::string mMessage;
    std::shared_ptr<Detail::ValueState> mJsException;

    ConvertError() = default;
    ConvertError(std::string message, std::shared_ptr<Detail::ValueState> jsException = nullptr)
        : mMessage(std::move(message)), mJsException(std::move(jsException)) {}

    [[nodiscard]] auto message() const& noexcept -> const std::string& { return mMessage; }
    [[nodiscard]] auto message() && -> std::string { return std::move(mMessage); }

    [[nodiscard]] static auto typeMismatch(std::string_view expected, std::string_view got)
        -> ConvertError;
    [[nodiscard]] static auto outOfRange(std::string_view expected, std::string_view got)
        -> ConvertError;
    [[nodiscard]] static auto forArgument(ConvertError inner, std::size_t index,
                                          std::string_view argName, std::string_view functionName)
        -> ConvertError;
    [[nodiscard]] static auto forElement(ConvertError inner, std::size_t index) -> ConvertError;
    [[nodiscard]] static auto forKey(ConvertError inner, std::string_view key) -> ConvertError;
};

template <class T> using Result = std::expected<T, ConvertError>;

class JsException : public std::exception {
public:
    explicit JsException(std::string message,
                         std::shared_ptr<Detail::ValueState> jsException = nullptr)
        : mMessage(std::move(message)), mJsException(std::move(jsException)) {}
    explicit JsException(ConvertError error)
        : mMessage(std::move(error.mMessage)), mJsException(std::move(error.mJsException)) {}

    [[nodiscard]] auto what() const noexcept -> const char* override { return mMessage.c_str(); }
    [[nodiscard]] auto message() const noexcept -> const std::string& { return mMessage; }
    [[nodiscard]] auto jsException() const noexcept -> const std::shared_ptr<Detail::ValueState>& {
        return mJsException;
    }

private:
    std::string mMessage;
    std::shared_ptr<Detail::ValueState> mJsException;
};

} // namespace Alka
