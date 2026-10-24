#pragma once

#include <Alka/Convert/Converter.hpp>

#include <v8.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ratio>
#include <string>
#include <type_traits>
#include <utility>

// durations are JS numbers of milliseconds
// (what setTimeout and Date arithmetic use); system_clock time points are JS Dates.
//   void wait(std::chrono::milliseconds timeout);       // JS: wait(250)
//   std::chrono::system_clock::time_point modified();   // JS gets a Date

namespace Alka {

namespace Detail {

template <class Rep, class Period>
[[nodiscard]] auto durationFromMilliseconds(const double ms)
    -> Result<std::chrono::duration<Rep, Period>> {
    using D = std::chrono::duration<Rep, Period>;
    if (!std::isfinite(ms)) {
        return std::unexpected(
            ConvertError::typeMismatch("finite number of milliseconds", "non-finite number"));
    }
    using PerMs = std::ratio_divide<std::milli, Period>; // ticks per millisecond
    const auto cannot = [&] {
        return std::unexpected(
            ConvertError{"this duration's unit cannot represent " + std::to_string(ms) + " ms"});
    };
    const auto outOfRange = [] {
        return std::unexpected(ConvertError{"this duration is out of range"});
    };
    if constexpr (std::is_floating_point_v<Rep>) {
        return D(static_cast<Rep>(ms * static_cast<double>(PerMs::num) /
                                  static_cast<double>(PerMs::den)));
    } else if (std::trunc(ms) == ms && std::abs(ms) <= 9007199254740991.0) {
        const auto whole = static_cast<std::int64_t>(ms);
        constexpr auto kMax = (std::numeric_limits<std::int64_t>::max)();
        if (whole > kMax / PerMs::num || whole < -(kMax / PerMs::num)) {
            return outOfRange();
        }
        const std::int64_t scaled = whole * static_cast<std::int64_t>(PerMs::num);
        if (scaled % PerMs::den != 0) {
            return cannot();
        }
        const std::int64_t ticks = scaled / PerMs::den;
        if (std::cmp_greater(ticks, (std::numeric_limits<Rep>::max)()) ||
            std::cmp_less(ticks, (std::numeric_limits<Rep>::min)())) {
            return outOfRange();
        }
        return D(static_cast<Rep>(ticks));
    } else {
        const double ticks = ms * static_cast<double>(PerMs::num) / static_cast<double>(PerMs::den);
        if (std::trunc(ticks) != ticks) {
            return cannot();
        }
        if (std::abs(ticks) > 9007199254740991.0 ||
            ticks > static_cast<double>((std::numeric_limits<Rep>::max)()) ||
            ticks < static_cast<double>((std::numeric_limits<Rep>::min)())) {
            return outOfRange();
        }
        return D(static_cast<Rep>(ticks));
    }
}

template <class Rep, class Period>
[[nodiscard]] auto durationToMilliseconds(const std::chrono::duration<Rep, Period> value)
    -> double {
    return std::chrono::duration<double, std::milli>(value).count();
}

} // namespace Detail

template <class Rep, class Period> struct Converter<std::chrono::duration<Rep, Period>> {
    using D = std::chrono::duration<Rep, Period>;
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<D> {
        if (value.IsEmpty() || !value->IsNumber()) {
            return std::unexpected(
                ConvertError::typeMismatch("number (milliseconds)", describeJsValue(value)));
        }
        return Detail::durationFromMilliseconds<Rep, Period>(value.As<v8::Number>()->Value());
    }
    static auto toJs(ConvertCtx& cx, const D& value) -> v8::Local<v8::Value> {
        return v8::Number::New(cx.mIsolate, Detail::durationToMilliseconds(value));
    }
};

template <class D> struct Converter<std::chrono::time_point<std::chrono::system_clock, D>> {
    using Point = std::chrono::time_point<std::chrono::system_clock, D>;
    static auto fromJs(ConvertCtx&, const v8::Local<v8::Value> value) -> Result<Point> {
        double ms = 0;
        if (!value.IsEmpty() && value->IsDate()) {
            ms = value.As<v8::Date>()->ValueOf();
            if (std::isnan(ms)) {
                return std::unexpected(ConvertError{"an invalid Date"});
            }
        } else if (!value.IsEmpty() && value->IsNumber()) {
            ms = value.As<v8::Number>()->Value();
        } else {
            return std::unexpected(ConvertError::typeMismatch("Date", describeJsValue(value)));
        }
        auto sinceEpoch = Detail::durationFromMilliseconds<typename D::rep, typename D::period>(ms);
        if (!sinceEpoch) {
            return std::unexpected(std::move(sinceEpoch).error());
        }
        return Point(*sinceEpoch);
    }
    static auto toJs(ConvertCtx& cx, const Point& value) -> v8::Local<v8::Value> {
        v8::Local<v8::Value> date;
        if (!v8::Date::New(cx.mContext, Detail::durationToMilliseconds(value.time_since_epoch()))
                 .ToLocal(&date)) {
            return v8::Undefined(cx.mIsolate); // (execution is terminating)
        }
        return date;
    }
};

} // namespace Alka
