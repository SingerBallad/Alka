#include "Support/V8Fixture.hpp"

#include <Alka/Converter.hpp>
#include <Alka/Converters/Primitives.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

namespace ConvertTest {

enum class Color { Red, Green = 5, Blue };

template<class T>
Alka::Result<T> evalFromJs(AlkaTest::V8Fixture& fx, std::string_view source) {
    return fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) -> Alka::Result<T> {
        Alka::ConvertCtx cx{isolate, context};
        v8::TryCatch tryCatch(isolate);
        v8::Local<v8::Value> value;
        if (!AlkaTest::V8Fixture::compileAndRun(isolate, context, source).ToLocal(&value)) {
            return std::unexpected(Alka::ConvertError{"script threw"});
        }
        return Alka::fromJs<T>(cx, value);
    });
}

bool messageContains(const auto& result, std::string_view needle) {
    return !result.has_value() && result.error().message().find(needle) != std::string::npos;
}

}  // namespace ConvertTest

TEST_CASE("int32 conversions") {
    AlkaTest::V8Fixture fx;
    CHECK(ConvertTest::evalFromJs<int>(fx, "42").value() == 42);
    CHECK(ConvertTest::evalFromJs<int>(fx, "-7").value() == -7);
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "3.5"), "integer"));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "'x'"),
                                       "expected number, got string"));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "NaN"), "integer"));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "1/0"), "integer"));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<std::int8_t>(fx, "200"),
                                       "cannot represent"));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<std::uint32_t>(fx, "-1"),
                                       "cannot represent"));
    CHECK(ConvertTest::evalFromJs<std::uint32_t>(fx, "4294967295").value() == 4294967295U);
}

TEST_CASE("int64 <-> bigint") {
    AlkaTest::V8Fixture fx;
    CHECK(ConvertTest::evalFromJs<std::int64_t>(fx, "123n").value() == 123);
    CHECK(ConvertTest::evalFromJs<std::int64_t>(fx, "-9223372036854775808n").value() ==
          (std::numeric_limits<std::int64_t>::min)());
    CHECK(ConvertTest::evalFromJs<std::int64_t>(fx, "12").value() == 12);
    CHECK(ConvertTest::messageContains(
        ConvertTest::evalFromJs<std::int64_t>(fx, "9007199254740993"), "bigint"));
    CHECK(ConvertTest::messageContains(
        ConvertTest::evalFromJs<std::uint64_t>(fx, "-1n"), "cannot represent"));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "5n"),
                                       "expected number, got bigint"));

    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        CHECK(Alka::toJs(cx, std::int64_t{5})->IsBigInt());
        CHECK(Alka::toJs(cx, std::uint64_t{5})->IsBigInt());
        CHECK(Alka::toJs(cx, 5)->IsInt32());
    });
}

TEST_CASE("bool is strict") {
    AlkaTest::V8Fixture fx;
    CHECK(ConvertTest::evalFromJs<bool>(fx, "true").value());
    CHECK_FALSE(ConvertTest::evalFromJs<bool>(fx, "false").value());
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<bool>(fx, "1"),
                                       "expected boolean, got number"));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<bool>(fx, "null"),
                                       "expected boolean, got null"));
}

TEST_CASE("floating point") {
    AlkaTest::V8Fixture fx;
    CHECK(ConvertTest::evalFromJs<double>(fx, "0.5").value() == 0.5);
    CHECK(std::isnan(ConvertTest::evalFromJs<double>(fx, "NaN").value()));
    CHECK(std::isinf(ConvertTest::evalFromJs<float>(fx, "Infinity").value()));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<double>(fx, "'0.5'"),
                                       "expected number, got string"));
}

TEST_CASE("strings roundtrip utf-8") {
    AlkaTest::V8Fixture fx;
    CHECK(ConvertTest::evalFromJs<std::string>(fx, "'héllo \\u{1F680}'").value() ==
          "héllo \U0001F680");
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<std::string>(fx, "42"),
                                       "expected string, got number"));

    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        CHECK(Alka::toJs(cx, std::string("abc"))->IsString());
        CHECK(Alka::toJs(cx, "literal")->IsString());
        CHECK(Alka::toJs(cx, "你好é"sv)->IsString());

        // toJs -> fromJs roundtrip
        const auto round = Alka::fromJs<std::string>(cx, Alka::toJs(cx, "你好é"sv));
        CHECK(round.value() == "你好é");
    });
}

TEST_CASE("enums convert as validated numbers") {
    AlkaTest::V8Fixture fx;
    CHECK(ConvertTest::evalFromJs<ConvertTest::Color>(fx, "5").value() == ConvertTest::Color::Green);
    CHECK(ConvertTest::evalFromJs<ConvertTest::Color>(fx, "0").value() == ConvertTest::Color::Red);
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<ConvertTest::Color>(fx, "42"),
                                       "not a valid Color value"));
    CHECK(ConvertTest::messageContains(ConvertTest::evalFromJs<ConvertTest::Color>(fx, "'Red'"),
                                       "expected number"));

    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto v = Alka::toJs(cx, ConvertTest::Color::Blue);
        CHECK(v->IsNumber());
        CHECK(Alka::fromJs<ConvertTest::Color>(cx, v).value() == ConvertTest::Color::Blue);
    });
}

// concept surface
static_assert(Alka::JsConvertible<int>);
static_assert(Alka::JsConvertible<std::string>);
static_assert(Alka::JsConvertible<ConvertTest::Color>);
static_assert(Alka::ToJsConvertible<std::string_view>);
static_assert(!Alka::FromJsConvertible<std::string_view>);
static_assert(Alka::ToJsConvertible<const char*>);
static_assert(!Alka::FromJsConvertible<void*>);
static_assert(!Alka::ToJsConvertible<AlkaTest::V8Fixture>);
