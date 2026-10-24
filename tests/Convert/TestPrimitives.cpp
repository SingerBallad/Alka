#include "Support/V8Fixture.hpp"

#include <Alka/Convert/Converter.hpp>
#include <Alka/Convert/Primitives.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

namespace ConvertTest {

enum class Color { Red, Green = 5, Blue };

template <class T> Alka::Result<T> evalFromJs(AlkaTest::V8Fixture& fx, std::string_view source) {
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

} // namespace ConvertTest

TEST(Primitives, Int32Conversions) {
    AlkaTest::V8Fixture fx;
    EXPECT_EQ(ConvertTest::evalFromJs<int>(fx, "42").value(), 42);
    EXPECT_EQ(ConvertTest::evalFromJs<int>(fx, "-7").value(), -7);
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "3.5"), "integer"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "'x'"),
                                             "expected number, got string"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "NaN"), "integer"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "1/0"), "integer"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<std::int8_t>(fx, "200"),
                                             "cannot represent"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<std::uint32_t>(fx, "-1"),
                                             "cannot represent"));
    EXPECT_EQ(ConvertTest::evalFromJs<std::uint32_t>(fx, "4294967295").value(), 4294967295U);
}

TEST(Primitives, Int64RoundTripAsBigint) {
    AlkaTest::V8Fixture fx;
    EXPECT_EQ(ConvertTest::evalFromJs<std::int64_t>(fx, "123n").value(), 123);
    EXPECT_EQ(ConvertTest::evalFromJs<std::int64_t>(fx, "-9223372036854775808n").value(),
              (std::numeric_limits<std::int64_t>::min)());
    EXPECT_EQ(ConvertTest::evalFromJs<std::int64_t>(fx, "12").value(), 12);
    EXPECT_TRUE(ConvertTest::messageContains(
        ConvertTest::evalFromJs<std::int64_t>(fx, "9007199254740993"), "bigint"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<std::uint64_t>(fx, "-1n"),
                                             "cannot represent"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<int>(fx, "5n"),
                                             "expected number, got bigint"));

    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        EXPECT_TRUE(Alka::toJs(cx, std::int64_t{5})->IsBigInt());
        EXPECT_TRUE(Alka::toJs(cx, std::uint64_t{5})->IsBigInt());
        EXPECT_TRUE(Alka::toJs(cx, 5)->IsInt32());
    });
}

TEST(Primitives, BoolIsStrict) {
    AlkaTest::V8Fixture fx;
    EXPECT_TRUE(ConvertTest::evalFromJs<bool>(fx, "true").value());
    EXPECT_FALSE(ConvertTest::evalFromJs<bool>(fx, "false").value());
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<bool>(fx, "1"),
                                             "expected boolean, got number"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<bool>(fx, "null"),
                                             "expected boolean, got null"));
}

TEST(Primitives, FloatingPoint) {
    AlkaTest::V8Fixture fx;
    EXPECT_EQ(ConvertTest::evalFromJs<double>(fx, "0.5").value(), 0.5);
    EXPECT_TRUE(std::isnan(ConvertTest::evalFromJs<double>(fx, "NaN").value()));
    EXPECT_TRUE(std::isinf(ConvertTest::evalFromJs<float>(fx, "Infinity").value()));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<double>(fx, "'0.5'"),
                                             "expected number, got string"));
}

TEST(Primitives, StringsRoundtripUtf8) {
    AlkaTest::V8Fixture fx;
    EXPECT_EQ(ConvertTest::evalFromJs<std::string>(fx, "'héllo \\u{1F680}'").value(),
              "héllo \U0001F680");
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<std::string>(fx, "42"),
                                             "expected string, got number"));

    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        EXPECT_TRUE(Alka::toJs(cx, std::string("abc"))->IsString());
        EXPECT_TRUE(Alka::toJs(cx, "literal")->IsString());
        EXPECT_TRUE(Alka::toJs(cx, "你好é"sv)->IsString());

        const auto round = Alka::fromJs<std::string>(cx, Alka::toJs(cx, "你好é"sv));
        EXPECT_EQ(round.value(), "你好é");
    });
}

TEST(Primitives, UTF8TranscodingEdgeCases) {
    AlkaTest::V8Fixture fx;
    EXPECT_EQ(ConvertTest::evalFromJs<std::string>(fx, "String.fromCharCode(0xE9) + 'a'").value(),
              "\xC3\xA9"
              "a");
    EXPECT_EQ(ConvertTest::evalFromJs<std::string>(fx, "String.fromCharCode(0xD800) + 'x'").value(),
              "\xEF\xBF\xBD"
              "x");
    EXPECT_EQ(
        ConvertTest::evalFromJs<std::string>(fx, "String.fromCharCode(0xD83D, 0xDE80)").value(),
        "\xF0\x9F\x9A\x80");
    EXPECT_TRUE(ConvertTest::evalFromJs<std::string>(fx, "''").value().empty());
}

TEST(Primitives, UTF16StringsWideStringsAndPaths) {
    AlkaTest::V8Fixture fx;
    EXPECT_EQ(ConvertTest::evalFromJs<std::u16string>(fx, "'abc'").value(), u"abc");
    EXPECT_EQ(ConvertTest::evalFromJs<std::u16string>(fx, "'你好\\u{1F680}'").value(),
              u"你好\U0001F680");
    EXPECT_EQ(ConvertTest::evalFromJs<std::wstring>(fx, "'é\\u{1F680}'").value(), L"é\U0001F680");
    EXPECT_EQ(ConvertTest::evalFromJs<std::filesystem::path>(fx, "'dir/文件.txt'").value(),
              std::filesystem::path(u8"dir/文件.txt"));
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<std::u16string>(fx, "1"),
                                             "expected string, got number"));

    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto check = [&](v8::Local<v8::Value> value, std::string_view expected) {
            EXPECT_EQ(Alka::fromJs<std::string>(cx, value).value(), expected);
        };
        check(Alka::toJs(cx, std::u16string(u"你好\U0001F680")), "你好\U0001F680");
        check(Alka::toJs(cx, std::u16string_view(u"ab")), "ab");
        check(Alka::toJs(cx, std::wstring(L"é\U0001F680")), "é\U0001F680");
        check(Alka::toJs(cx, std::filesystem::path(u8"dir/文件.txt")), "dir/文件.txt");
    });
}

TEST(Primitives, MonostateAndNullptrTStandForUndefinedAndNull) {
    AlkaTest::V8Fixture fx;
    EXPECT_TRUE(ConvertTest::evalFromJs<std::monostate>(fx, "undefined").has_value());
    EXPECT_TRUE(ConvertTest::evalFromJs<std::nullptr_t>(fx, "null").has_value());
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<std::monostate>(fx, "0"),
                                             "expected undefined, got number"));
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        EXPECT_TRUE(Alka::toJs(cx, std::monostate{})->IsUndefined());
        EXPECT_TRUE(Alka::toJs(cx, nullptr)->IsNull());
    });
}

TEST(Primitives, EnumsConvertAsValidatedNumbers) {
    AlkaTest::V8Fixture fx;
    EXPECT_EQ(ConvertTest::evalFromJs<ConvertTest::Color>(fx, "5").value(),
              ConvertTest::Color::Green);
    EXPECT_EQ(ConvertTest::evalFromJs<ConvertTest::Color>(fx, "0").value(),
              ConvertTest::Color::Red);
    EXPECT_TRUE(ConvertTest::messageContains(ConvertTest::evalFromJs<ConvertTest::Color>(fx, "42"),
                                             "not a valid Color value"));
    EXPECT_TRUE(ConvertTest::messageContains(
        ConvertTest::evalFromJs<ConvertTest::Color>(fx, "'Red'"), "expected number"));

    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto v = Alka::toJs(cx, ConvertTest::Color::Blue);
        EXPECT_TRUE(v->IsNumber());
        EXPECT_EQ(Alka::fromJs<ConvertTest::Color>(cx, v).value(), ConvertTest::Color::Blue);
    });
}

static_assert(Alka::JsConvertible<int>);
static_assert(Alka::JsConvertible<std::string>);
static_assert(Alka::JsConvertible<ConvertTest::Color>);
static_assert(Alka::ToJsConvertible<std::string_view>);
static_assert(!Alka::FromJsConvertible<std::string_view>);
static_assert(Alka::ToJsConvertible<const char*>);
static_assert(!Alka::FromJsConvertible<void*>);
static_assert(!Alka::ToJsConvertible<AlkaTest::V8Fixture>);
