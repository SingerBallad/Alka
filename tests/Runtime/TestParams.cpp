#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ParamTest {

inline int viewLength(std::string_view text) {
    return static_cast<int>(text.size());
}
inline std::string cstr(const char* text) {
    return text == nullptr ? "<null>" : text;
}
inline int sumSpan(std::span<const int> values) {
    return std::accumulate(values.begin(), values.end(), 0);
}
inline int byteCount(std::span<const std::uint8_t> bytes) {
    return static_cast<int>(bytes.size());
}

inline std::string join(std::string_view sep, Alka::Rest<std::string> parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) {
            out += sep;
        }
        out += part;
    }
    return out;
}
inline int countAll(Alka::Rest<Alka::Value> values) {
    return static_cast<int>(values.size());
}

inline std::expected<int, std::string> parsePositive(int value) {
    if (value <= 0) {
        return std::unexpected("not positive: " + std::to_string(value));
    }
    return value;
}
inline std::expected<void, std::error_code> mayFail(bool fail) {
    if (fail) {
        return std::unexpected(std::make_error_code(std::errc::permission_denied));
    }
    return {};
}
inline std::expected<int, Alka::ConvertError> callAndReport(Alka::Function<int()> fn) {
    return fn.tryCall();
}

struct Counter {
    int mCount = 0;
    int add(Alka::Rest<int> values) {
        for (const int v : values) {
            mCount += v;
        }
        return mCount;
    }
    std::expected<Counter, std::string> cloneIfPositive() const {
        if (mCount <= 0) {
            return std::unexpected("empty counter");
        }
        return *this;
    }
};

inline Alka::Promise<int> gPending;
inline Alka::Promise<int> later() {
    gPending = Alka::Promise<int>::create();
    return gPending;
}
inline Alka::Promise<> done() {
    auto promise = Alka::Promise<>::create();
    (void)promise.resolve();
    return promise;
}

struct Mod;

} // namespace ParamTest

template <> struct Alka::Bind<ParamTest::Counter> : Class<ParamTest::Counter> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&ParamTest::Counter::add>(Alka::arg("values")),
                      Alka::method<&ParamTest::Counter::cloneIfPositive>(),
                      Alka::prop<&ParamTest::Counter::mCount>());
};

template <> struct Alka::Bind<ParamTest::Mod> : Module<"params"> {
    static constexpr auto members = Alka::members(
        Alka::type<ParamTest::Counter>, Alka::fn<&ParamTest::viewLength>(arg("text")),
        Alka::fn<&ParamTest::cstr>(arg("text")), Alka::fn<&ParamTest::sumSpan>(arg("values")),
        Alka::fn<&ParamTest::byteCount>(arg("bytes")),
        Alka::fn<&ParamTest::join>(arg("sep"), arg("parts")),
        Alka::fn<&ParamTest::countAll>(arg("values")),
        Alka::fn<"pick", [](int) { return std::string("one"); },
                 [](int, Rest<int>) { return std::string("many"); }>(),
        Alka::fn<&ParamTest::parsePositive>(arg("value")),
        Alka::fn<&ParamTest::mayFail>(arg("fail")), Alka::fn<&ParamTest::callAndReport>(arg("fn")),
        Alka::fn<&ParamTest::later>(), Alka::fn<&ParamTest::done>());
};

namespace {

struct ParamFixture : AlkaTest::V8Fixture {
    ParamFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<ParamTest::Mod>(context);
        });
    }

    void drainMicrotasks() {
        run([](v8::Isolate* isolate, v8::Local<v8::Context>) {
            isolate->PerformMicrotaskCheckpoint();
        });
    }
};

} // namespace

TEST(Params, StringViewConstCharAndSpanParameters) {
    ParamFixture fx;
    EXPECT_EQ(fx.evalInt("params.viewLength('héllo')"), 6); // UTF-8 bytes
    EXPECT_EQ(fx.evalString("params.cstr('abc')"), "abc");
    EXPECT_EQ(fx.evalString("params.cstr(null)"), "<null>");
    EXPECT_EQ(fx.evalInt("params.sumSpan([1, 2, 3, 4])"), 10);
    EXPECT_EQ(fx.evalInt("params.byteCount(new Uint8Array(7))"), 7);
    EXPECT_EQ(fx.evalString("try { params.sumSpan([1, 'x']) } catch (e) { e.message }"),
              "arg 1 (values) of params.sumSpan: element 1: expected number, got string");
}

TEST(Params, RestTCollectsTheRemainingArguments) {
    ParamFixture fx;
    EXPECT_EQ(fx.evalString("params.join('-', 'a', 'b', 'c')"), "a-b-c");
    EXPECT_EQ(fx.evalString("params.join('-')"), "");
    EXPECT_EQ(fx.evalInt("params.countAll(1, 'x', null, {}, [])"), 5);
    EXPECT_EQ(fx.evalInt("params.countAll()"), 0);
    EXPECT_EQ(fx.evalString("try { params.join('-', 'a', 2) } catch (e) { e.message }"),
              "arg 2 (parts) of params.join: element 1: expected string, got number");
    EXPECT_EQ(fx.evalInt("new params.Counter().add(1, 2, 3)"), 6);
    EXPECT_EQ(fx.evalString("params.pick(1)"), "one");
    EXPECT_EQ(fx.evalString("params.pick(1, 2, 3, 4)"), "many");
}

TEST(Params, StdExpectedReturnsAValueOrThrowsItsError) {
    ParamFixture fx;
    EXPECT_EQ(fx.evalInt("params.parsePositive(4)"), 4);
    EXPECT_EQ(fx.evalString("try { params.parsePositive(-1) } catch (e) { e.message }"),
              "not positive: -1");
    EXPECT_TRUE(fx.evalBool("params.mayFail(false) === undefined"));
    EXPECT_TRUE(
        fx.evalBool("(() => { try { params.mayFail(true); } catch (e) {"
                    " return e instanceof Error && e.message.length > 0; } return false; })()"));
    EXPECT_TRUE(fx.evalBool("(() => { const err = new TypeError('inner'); try {"
                            " params.callAndReport(() => { throw err; }); } catch (e) {"
                            " return e === err; } return false; })()"));
    EXPECT_EQ(fx.evalInt("params.callAndReport(() => 9)"), 9);
    EXPECT_EQ(
        fx.evalInt("(() => { const c = new params.Counter(); c.add(2);"
                   " const d = c.cloneIfPositive(); d.add(1); return c.count * 10 + d.count; })()"),
        23);
    EXPECT_EQ(
        fx.evalString("try { new params.Counter().cloneIfPositive() } catch (e) { e.message }"),
        "empty counter");
}

TEST(Params, PromiseTSettlesFromCxx) {
    ParamFixture fx;
    fx.evalString("globalThis.result = 'pending';"
                  " params.later().then(v => { result = 'got ' + v; }); 'ok'");
    fx.drainMicrotasks();
    EXPECT_EQ(fx.evalString("result"), "pending");
    EXPECT_TRUE(ParamTest::gPending.resolve(42).has_value());
    fx.drainMicrotasks();
    EXPECT_EQ(fx.evalString("result"), "got 42");

    fx.evalString("params.later().catch(e => { result = e.message; }); 'ok'");
    EXPECT_TRUE(ParamTest::gPending.reject("load failed").has_value());
    fx.drainMicrotasks();
    EXPECT_EQ(fx.evalString("result"), "load failed");

    fx.evalString("params.done().then(v => { result = typeof v; }); 'ok'");
    fx.drainMicrotasks();
    EXPECT_EQ(fx.evalString("result"), "undefined");
    ParamTest::gPending = {};
}

TEST(Params, ExceptionValueExposesTheThrownJSValueToCxx) {
    ParamFixture fx;
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        v8::Local<v8::Value> fnValue =
            AlkaTest::V8Fixture::compileAndRun(
                isolate, context, "(() => { const e = new Error('bad'); e.code = 7; throw e; })")
                .ToLocalChecked();
        Alka::ConvertCtx cx{isolate, context};
        const auto fn = Alka::fromJs<Alka::Function<void()>>(cx, fnValue);
        ASSERT_TRUE(fn.has_value());
        const auto result = fn->tryCall();
        ASSERT_FALSE(result.has_value());
        const auto thrown = Alka::exceptionValue(result.error());
        ASSERT_TRUE(thrown.valid());
        const auto object = thrown.to<Alka::Object>();
        ASSERT_TRUE(object.has_value());
        EXPECT_EQ(object->getAs<int>("code").value(), 7);
        EXPECT_EQ(object->getAs<std::string>("message").value(), "bad");
    });
}

TEST(Params, DtsRendersTheNewParameterAndReturnShapes) {
    const std::string dts = Alka::generateDts<ParamTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("function viewLength(text: string): number;"));
    EXPECT_THAT(dts, testing::HasSubstr("function cstr(text: string | null): string;"));
    EXPECT_THAT(dts,
                testing::HasSubstr("function sumSpan(values: number[] | Int32Array): number;"));
    EXPECT_THAT(dts, testing::HasSubstr(
                         "function byteCount(bytes: ArrayBuffer | ArrayBufferView): number;"));
    EXPECT_THAT(dts, testing::HasSubstr("function join(sep: string, ...parts: string[]): string;"));
    EXPECT_THAT(dts, testing::HasSubstr("function countAll(...values: unknown[]): number;"));
    EXPECT_THAT(dts, testing::HasSubstr("add(...values: number[]): number;"));
    EXPECT_THAT(dts, testing::HasSubstr("function parsePositive(value: number): number;"));
    EXPECT_THAT(dts, testing::HasSubstr("function mayFail(fail: boolean): void;"));
    EXPECT_THAT(dts, testing::HasSubstr("cloneIfPositive(): params.Counter;"));
    EXPECT_THAT(dts, testing::HasSubstr("function later(): Promise<number>;"));
    EXPECT_THAT(dts, testing::HasSubstr("function done(): Promise<void>;"));
}
