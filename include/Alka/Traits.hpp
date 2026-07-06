#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace Alka {

template<class T>
concept Enum = std::is_enum_v<T>;

namespace Detail {

template<class...>
inline constexpr auto AlwaysFalse = false;

template<class F>
struct FnTraits {
    static_assert(AlwaysFalse<F>,
                  "Alka: unsupported callable shape (ref-qualified member functions, generic "
                  "lambdas and overloaded call operators cannot be bound directly)");
};

template<class R, class... Args>
struct FnTraits<R (*)(Args...)> {
    using Ret = R;
    using Class = void;
    using ArgsTuple = std::tuple<Args...>;
    static constexpr auto isMemberFunction = false;
    static constexpr auto isConst = false;
    static constexpr auto arity = sizeof...(Args);
};

template<class R, class... Args>
struct FnTraits<R (*)(Args...) noexcept> : FnTraits<R (*)(Args...)> {};

template<class C, class R, class... Args>
struct FnTraits<R (C::*)(Args...)> {
    using Ret = R;
    using Class = C;
    using ArgsTuple = std::tuple<Args...>;
    static constexpr auto isMemberFunction = true;
    static constexpr auto isConst = false;
    static constexpr auto arity = sizeof...(Args);
};

template<class C, class R, class... Args>
struct FnTraits<R (C::*)(Args...) noexcept> : FnTraits<R (C::*)(Args...)> {};

template<class C, class R, class... Args>
struct FnTraits<R (C::*)(Args...) const> : FnTraits<R (C::*)(Args...)> {
    static constexpr auto isConst = true;
};

template<class C, class R, class... Args>
struct FnTraits<R (C::*)(Args...) const noexcept> : FnTraits<R (C::*)(Args...) const> {};

template<class F>
concept StatelessCallable =
    std::is_class_v<F> && std::is_empty_v<F> && requires { &F::operator(); };

template<auto F>
[[nodiscard]] consteval auto fnTraitsProbe() {
    using D = decltype(F);
    if constexpr (std::is_member_function_pointer_v<D>) {
        return FnTraits<D>{};
    } else if constexpr (std::is_pointer_v<D> && std::is_function_v<std::remove_pointer_t<D>>) {
        return FnTraits<D>{};
    } else if constexpr (StatelessCallable<D>) {
        return FnTraits<decltype(&D::operator())>{};
    } else {
        static_assert(AlwaysFalse<D>,
                      "Alka: this value is not bindable (expected a function pointer, a member "
                      "pointer or a captureless lambda)");
    }
}

}  // namespace Detail

template<auto F>
using FnTraitsOf = decltype(Detail::fnTraitsProbe<F>());

template<auto F>
inline constexpr bool isMemberCallable = std::is_member_function_pointer_v<decltype(F)>;

template<class T>
concept BindableCallableValue = std::is_member_function_pointer_v<T> ||
                                (std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>) ||
                                Detail::StatelessCallable<T>;

}  // namespace Alka
