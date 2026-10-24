#pragma once

#include <Alka/Config.hpp>

#include <concepts>
#include <cstdint>
#include <type_traits>

// Per-declaration behavior specs that steer how a call's result is handed to
// JS and what a call keeps alive:
//   Alka::method<&Scene::find>(Alka::policy::borrow)
//   Alka::method<&Node::parent>(Alka::policy::internalRef)
//   Alka::method<&Scene::add>(Alka::keepAlive<0>)        // scene keeps arg 0 alive
//   Alka::fn<&fileSize>(Alka::int64As<Alka::Int64Mode::Number>)

namespace Alka {

// How a T& / T* (or a value) result of a bound class is surfaced to JS.
enum class ReturnPolicy : std::uint8_t {
    Automatic,   // T*/T& borrow (shared when the object is shared_from_this-managed),
                 // by-value / unique_ptr move in, shared_ptr shares; fields: InternalRef
    Borrow,      // reference the C++ object; C++ keeps ownership
    Copy,        // copy the referenced object into a JS-owned one
    Move,        // move from the referenced object into a JS-owned one
    Take,        // T* transfers ownership to JS (it will delete it)
    InternalRef, // borrow, and the result keeps `this` alive (it points into it)
};

template <ReturnPolicy P> struct PolicySpec {
    static constexpr ReturnPolicy value = P;
};

namespace policy {
inline constexpr PolicySpec<ReturnPolicy::Borrow> borrow{};
inline constexpr PolicySpec<ReturnPolicy::Copy> copy{};
inline constexpr PolicySpec<ReturnPolicy::Move> move{};
inline constexpr PolicySpec<ReturnPolicy::Take> take{};
inline constexpr PolicySpec<ReturnPolicy::InternalRef> internalRef{};
} // namespace policy

// After the call, `this` holds a reference to JS argument `Index`.
// (e.g. a container that stores a pointer to what it was given). Methods only.
template <int Index> struct KeepAliveSpec {
    static_assert(Index >= 0, "Alka: keepAlive<> takes a JS argument index");
    static constexpr int index = Index;
};
template <int Index> inline constexpr KeepAliveSpec<Index> keepAlive{};

enum class Int64Mode : std::uint8_t {
    BigInt, // lossless: always a bigint
    Number, // always a number (rounds beyond 2^53)
    Auto,   // a number when exactly representable, else a bigint
};

inline constexpr auto kDefaultInt64Mode = static_cast<Int64Mode>(ALKA_INT64_MODE);

template <Int64Mode M> struct Int64Spec {
    static constexpr Int64Mode value = M;
};
template <Int64Mode M> inline constexpr Int64Spec<M> int64As{};

struct NoFastTag {};
inline constexpr NoFastTag noFast{};

namespace Detail {

template <class T> inline constexpr bool isPolicySpec = false;
template <ReturnPolicy P> inline constexpr bool isPolicySpec<PolicySpec<P>> = true;

template <class T> inline constexpr bool isKeepAliveSpec = false;
template <int I> inline constexpr bool isKeepAliveSpec<KeepAliveSpec<I>> = true;

template <class T> inline constexpr bool isInt64Spec = false;
template <Int64Mode M> inline constexpr bool isInt64Spec<Int64Spec<M>> = true;

} // namespace Detail

template <class T>
concept IsPolicySpec = Detail::isPolicySpec<std::remove_cvref_t<T>>;
template <class T>
concept IsKeepAliveSpec = Detail::isKeepAliveSpec<std::remove_cvref_t<T>>;
template <class T>
concept IsInt64Spec = Detail::isInt64Spec<std::remove_cvref_t<T>>;
template <class T>
concept IsNoFast = std::same_as<std::remove_cvref_t<T>, NoFastTag>;

} // namespace Alka
