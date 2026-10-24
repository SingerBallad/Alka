#pragma once

#include <Alka/Binding/Policy.hpp>
#include <Alka/Config.hpp>
#include <Alka/Convert/Primitives.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>

#if ALKA_FAST_API
#include <v8-fast-api-calls.h>
#endif
#include <v8.h>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Alka::Detail {

#if ALKA_FAST_API_STATS
// Test hook: how many calls took the fast path.
inline std::atomic<std::uint64_t> gFastCalls{0};
#endif

#if !ALKA_FAST_API

// Fast paths disabled (ALKA_FAST_API=0 or no v8-fast-api-calls.h): every
// callable takes the regular path.
inline constexpr bool kFastApiUsable = false;

template <class T, class Owner, std::size_t I>
[[nodiscard]] constexpr auto fastFunctionOf() -> const v8::CFunction* {
    return nullptr;
}

#else

template <class O>
concept FastOptionsCarryIsolate = requires(O& options) { options.isolate; };
inline constexpr bool kFastApiUsable = FastOptionsCarryIsolate<v8::FastApiCallbackOptions>;

template <class O> [[nodiscard]] auto isolateOf(O& options) -> v8::Isolate* {
    if constexpr (FastOptionsCarryIsolate<O>) {
        return options.isolate;
    } else {
        return nullptr; // (unreachable: no fast paths are generated)
    }
}

template <class D> [[nodiscard]] consteval bool fastScalarCheck() {
    if constexpr (std::same_as<D, bool> || std::same_as<D, float> || std::same_as<D, double>) {
        return true;
    } else if constexpr (std::integral<D> && !std::same_as<D, char>) {
        return sizeof(D) <= 4;
    } else {
        return false;
    }
}

template <class D> inline constexpr bool isFastScalar = fastScalarCheck<D>();

template <class D>
inline constexpr bool isFastString =
    std::same_as<D, std::string> || std::same_as<D, std::string_view>;

template <class A>
inline constexpr bool isFastParam =
    !(std::is_reference_v<A> && !std::is_const_v<std::remove_reference_t<A>>) &&
    (isFastScalar<std::remove_cvref_t<A>> || isFastString<std::remove_cvref_t<A>>);

template <class R>
inline constexpr bool isFastReturn =
    std::is_void_v<R> || (!std::is_reference_v<R> && isFastScalar<std::remove_cv_t<R>>);

template <class A>
using FastParamOf = std::conditional_t<
    isFastString<std::remove_cvref_t<A>>, const v8::FastOneByteString&,
    std::conditional_t<std::same_as<std::remove_cvref_t<A>, bool>, bool, double>>;

template <class D>
using FastConvertedOf =
    std::conditional_t<std::same_as<D, std::string_view>, InlineUtf8,
                       std::conditional_t<std::same_as<D, std::string>, std::string, D>>;

template <class R> struct FastReturn {
    using D = std::remove_cv_t<R>;
    using Type = std::conditional_t<
        std::same_as<D, bool>, bool,
        std::conditional_t<std::floating_point<D>, double,
                           std::conditional_t<std::is_signed_v<D>, std::int32_t, std::uint32_t>>>;
};
template <> struct FastReturn<void> {
    using Type = void;
};
template <class R> using FastReturnOf = FastReturn<R>::Type;

template <class D>
[[nodiscard]] auto fastArg(const FastParamOf<D> raw) -> Result<FastConvertedOf<D>> {
    if constexpr (std::same_as<D, std::string_view>) {
        InlineUtf8 out;
        out.assign(reinterpret_cast<const std::uint8_t*>(raw.data), raw.length);
        return out;
    } else if constexpr (std::same_as<D, std::string>) {
        return latin1ToUtf8(reinterpret_cast<const std::uint8_t*>(raw.data), raw.length);
    } else if constexpr (std::same_as<D, bool>) {
        return raw;
    } else if constexpr (std::floating_point<D>) {
        return static_cast<D>(raw);
    } else {
        return integerFromNumber<D>(raw);
    }
}

template <class Desc> [[nodiscard]] consteval bool fastAllowed() {
    if constexpr (requires { Desc::fastAllowed(); }) {
        return Desc::fastAllowed();
    } else {
        return true;
    }
}

template <class T, class Desc> [[nodiscard]] consteval bool fastEligible() {
    if constexpr (!kFastApiUsable || !fastAllowed<Desc>()) {
        return false;
    } else if constexpr (!requires { Desc::callables; } || Desc::overloadCount != 1) {
        return false;
    } else {
        using FT = std::remove_cvref_t<decltype(std::get<0>(Desc::callables))>;
        using Args = CallArgsOfType<FT, T>;
        using R = FnTraitsOfType<FT>::Ret;
        constexpr bool argsOk = []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (isFastParam<std::tuple_element_t<Is, Args>> && ... && true);
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
        return argsOk && isFastReturn<R> && !anySpecDefault<Desc>() && Desc::keepAliveCount == 0 &&
               Desc::returnPolicy() == ReturnPolicy::Automatic;
    }
}

inline void throwFast(v8::Isolate* isolate, const std::string_view message) {
    const v8::HandleScope scope(isolate);
    throwTypeError(isolate, message);
}

template <class Owner, std::size_t I, class T, auto F, class... A>
auto fastCall(v8::Local<v8::Value> receiver, FastParamOf<A>... raw,
              v8::FastApiCallbackOptions& options) -> FastReturnOf<typename FnTraitsOf<F>::Ret> {
    using R = FnTraitsOf<F>::Ret;
    using Args = std::tuple<A...>;
#if ALKA_FAST_API_STATS
    gFastCalls.fetch_add(1, std::memory_order_relaxed);
#endif
    const auto& site = kSite<Owner, I, CallSite::Kind::Call>;
    std::optional<std::tuple<Result<FastConvertedOf<std::remove_cvref_t<A>>>...>> holder;
    try {
        holder.emplace(fastArg<std::remove_cvref_t<A>>(raw)...);
    } catch (...) {
        const v8::HandleScope scope(isolateOf(options));
        throwCppException(isolateOf(options), std::current_exception(), site.mDisplayName);
        return FastReturnOf<R>{};
    }
    auto& converted = *holder;
    std::optional<std::pair<ConvertError, std::size_t>> failure;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (void)((std::get<Is>(converted).has_value()
                    ? false
                    : (failure.emplace(std::get<Is>(converted).error(), Is), true)) ||
               ...);
    }(std::index_sequence_for<A...>{});
    if (failure) {
        throwFast(isolateOf(options), site.argumentError(failure->first, failure->second));
        return FastReturnOf<R>{};
    }
    StoredTupleOf<Args> stored = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return StoredTupleOf<Args>{std::move(*std::get<Is>(converted))...};
    }(std::index_sequence_for<A...>{});

    try {
        constexpr auto seq = std::index_sequence_for<A...>{};
        if constexpr (std::is_void_v<T>) {
            (void)receiver;
            if constexpr (std::is_void_v<R>) {
                callFree<F, Args>(stored, seq);
            } else {
                return static_cast<FastReturnOf<R>>(callFree<F, Args>(stored, seq));
            }
        } else {
            auto self = unwrapObject<T>(receiver);
            if (!self) {
                throwFast(isolateOf(options), site.mDisplayName + ": " + self.error().message());
                return FastReturnOf<R>{};
            }
            // (alive for the call: a trampoline reached from it runs the C++ base)
            std::optional<BaseCallScope> baseCall;
            if constexpr (std::is_polymorphic_v<T>) {
                baseCall.emplace(dynamic_cast<const void*>(self->mPtr), DescAt<Owner, I>::name);
            }
            if constexpr (std::is_void_v<R>) {
                callOnSelf<F, Args>(self->mPtr, stored, seq);
            } else {
                return static_cast<FastReturnOf<R>>(callOnSelf<F, Args>(self->mPtr, stored, seq));
            }
        }
    } catch (...) {
        const v8::HandleScope scope(isolateOf(options));
        throwCppException(isolateOf(options), std::current_exception(), site.mDisplayName);
        return FastReturnOf<R>{};
    }
}

template <class Owner, std::size_t I, class T, auto F, class... A>
[[nodiscard]] auto fastFunctionFor(std::type_identity<std::tuple<A...>>) -> const v8::CFunction* {
    static const v8::CFunction function = v8::CFunction::Make(&fastCall<Owner, I, T, F, A...>);
    return &function;
}

template <class T, class Owner, std::size_t I>
[[nodiscard]] auto fastFunctionOf() -> const v8::CFunction* {
    using Desc = DescAt<Owner, I>;
    if constexpr (fastEligible<T, Desc>()) {
        using Args = CallArgsOfType<std::remove_cvref_t<decltype(std::get<0>(Desc::callables))>, T>;
        return fastFunctionFor<Owner, I, T, std::get<0>(Desc::callables)>(
            std::type_identity<Args>{});
    } else {
        return nullptr;
    }
}

#endif // ALKA_FAST_API

} // namespace Alka::Detail
