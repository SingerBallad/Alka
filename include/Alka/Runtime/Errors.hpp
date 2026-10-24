#pragma once

#include <Alka/Binding/Members.hpp>
#include <Alka/Binding/Meta.hpp>
#include <Alka/Convert/Converter.hpp>
#include <Alka/Core/FixedString.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Js/Value.hpp>

#include <v8.h>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

// C++ exception types as JS Error subclasses.
//
//   struct NotFound : std::runtime_error { using runtime_error::runtime_error; int mCode = 404; };
//
//   Alka::registerError<NotFound>("NotFound", [](const NotFound& e, Alka::Object& error) {
//       (void)error.set("code", e.mCode);                // optional extra fields
//   });
//   // ...or export the class from a module (also registers it):
//   Alka::error<"NotFound", NotFound>()                  // game.NotFound

namespace Alka {

namespace Detail {

using ErrorMatcher = bool (*)(const std::exception_ptr& error, ConvertCtx& cx,
                              v8::Local<v8::Function> errorClass, const void* decorate,
                              v8::Local<v8::Value>& out);

template <class E> using ErrorDecorate = std::function<void(const E&, Object&)>;

struct ErrorTypeInfo {
    std::string mName;
    const std::type_info* mType = nullptr;
    const std::type_info* mBase = nullptr;
    ErrorMatcher mMatch = nullptr;
    std::shared_ptr<const void> mDecorate;
};

void registerErrorType(ErrorTypeInfo info);

[[nodiscard]] auto errorClass(v8::Local<v8::Context> context, const std::type_info& type)
    -> v8::MaybeLocal<v8::Function>;

template <class E> [[nodiscard]] auto errorMessageOf(const E& error) -> std::string {
    if constexpr (std::is_base_of_v<std::exception, E>) {
        return error.what();
    } else if constexpr (requires { std::string(error.message()); }) {
        return std::string(error.message());
    } else {
        return std::string(typeid(E).name());
    }
}

template <class E>
bool matchError(const std::exception_ptr& error, ConvertCtx& cx,
                const v8::Local<v8::Function> errorClass, const void* decorate,
                v8::Local<v8::Value>& out) {
    try {
        std::rethrow_exception(error);
    } catch (const E& e) {
        v8::Local<v8::Value> argv[] = {v8Str(cx.mIsolate, errorMessageOf(e))};
        v8::Local<v8::Object> object;
        if (errorClass->NewInstance(cx.mContext, 1, argv).ToLocal(&object)) {
            if (decorate != nullptr) {
                try {
                    Object handle(cx, object);
                    (*static_cast<const ErrorDecorate<E>*>(decorate))(e, handle);
                } catch (...) {
                }
            }
            out = object;
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace Detail

template <class E, class Base = void> void registerError(const std::string_view jsName) {
    static_assert(std::is_void_v<Base> || std::is_base_of_v<Base, E>,
                  "Alka: registerError<E, Base>: Base must be a base class of E");
    Detail::registerErrorType(Detail::ErrorTypeInfo{std::string(jsName), &typeid(E),
                                                    std::is_void_v<Base> ? nullptr : &typeid(Base),
                                                    &Detail::matchError<E>, nullptr});
}

template <class E, class Base = void, class Decorate>
    requires std::invocable<Decorate&, const E&, Object&>
void registerError(const std::string_view jsName, Decorate decorate) {
    static_assert(std::is_void_v<Base> || std::is_base_of_v<Base, E>,
                  "Alka: registerError<E, Base>: Base must be a base class of E");
    Detail::registerErrorType(Detail::ErrorTypeInfo{
        std::string(jsName), &typeid(E), std::is_void_v<Base> ? nullptr : &typeid(Base),
        &Detail::matchError<E>,
        std::make_shared<const Detail::ErrorDecorate<E>>(std::move(decorate))});
}

template <FixedString Name, class E, class Base, class SpecTuple>
struct ErrorTypeDesc : Detail::DescBase<Name, SpecTuple> {
    static constexpr auto kind = MemberKind::ErrorType;
    using Type = E;
    using BaseType = Base;
};

template <FixedString Name, class E, class Base = void, AnySpec... Specs>
[[nodiscard]] constexpr auto error(Specs... specs) {
    static_assert(Detail::isIdentifier(Name.view()), "Alka: error<> name must be an identifier");
    return ErrorTypeDesc<Name, E, Base, std::tuple<Specs...>>{{std::tuple<Specs...>(specs...)}};
}

} // namespace Alka
