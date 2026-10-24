#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace FastTest {

inline int add(int a, int b) {
    return a + b;
}
inline double scale(double v, double f) {
    return v * f;
}
inline bool flip(bool b) {
    return !b;
}
inline std::uint32_t mask(std::uint32_t v) {
    return v & 0xFFu;
}
inline int checked(int v) {
    if (v < 0) {
        throw std::runtime_error("negative input");
    }
    return v;
}
inline int slowOnly(int v) {
    return v;
}
inline int byteLength(std::string_view text) {
    return static_cast<int>(text.size());
}
inline int count(const std::string& text, const int c) {
    return static_cast<int>(std::ranges::count(text, static_cast<char>(c)));
}
inline std::string gSeen;
inline void see(std::string_view text) {
    gSeen = text;
}

struct Counter {
    int mCount = 0;
    int bump(int by) { return mCount += by; }
};

class Meter {
public:
    virtual ~Meter() = default;
    virtual int read(const int x) { return x + 1; }
};

struct JsMeter : Meter, Alka::Overridable<Meter> {
    int read(const int x) override { return ALKA_OVERRIDE(int, Meter, read, x); }
};

inline int readOf(Meter& meter, const int x) {
    return meter.read(x);
}

struct Mod;

} // namespace FastTest

template <> struct Alka::Bind<FastTest::Counter> : Class<FastTest::Counter> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&FastTest::Counter::bump>(arg("by")));
};

template <> struct Alka::Bind<FastTest::Meter> : Class<FastTest::Meter> {
    using Trampoline = FastTest::JsMeter;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"read", &FastTest::Meter::read>(arg("x")));
};

template <> struct Alka::Bind<FastTest::Mod> : Module<"fast"> {
    static constexpr auto members = Alka::members(
        Alka::type<FastTest::Counter>, Alka::type<FastTest::Meter>, Alka::fn<&FastTest::readOf>(),
        Alka::fn<&FastTest::add>(arg("a"), arg("b")), Alka::fn<&FastTest::scale>(),
        Alka::fn<&FastTest::flip>(), Alka::fn<&FastTest::mask>(),
        Alka::fn<&FastTest::checked>(arg("v")), Alka::fn<&FastTest::slowOnly>(noFast),
        Alka::fn<&FastTest::byteLength>(arg("text")),
        Alka::fn<&FastTest::count>(arg("text"), arg("c")), Alka::fn<&FastTest::see>(arg("text")));
};

namespace {

struct FastFixture : AlkaTest::V8Fixture {
    FastFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<FastTest::Mod>(context);
        });
    }

    std::string optimizedCall(const std::string& name, const std::string& arg) {
        return evalString("%PrepareFunctionForOptimization(" + name + "); " + name + "(" + arg +
                          "); " + name + "(" + arg + "); %OptimizeFunctionOnNextCall(" + name +
                          "); String(" + name + "(" + arg + "))");
    }

    static std::uint64_t fastCalls() { return Alka::Detail::gFastCalls.load(); }
};

} // namespace

TEST(FastApi, OptimizedCallsTakeTheFastPathWithTheSameResults) {
    FastFixture fx;
    fx.evalString(
        "globalThis.sum = n => { let s = 0; for (let i = 0; i < n; ++i) s += fast.add(i, 1);"
        " return s; };"
        "globalThis.mix = x => fast.scale(x, 2) + (fast.flip(false) ? 1 : 0) + fast.mask(0x1FF);"
        "globalThis.bumpy = n => { const c = new fast.Counter(); let r = 0;"
        " for (let i = 0; i < n; ++i) r = c.bump(2); return r; }; 'ok'");
    const auto before = FastFixture::fastCalls();
    EXPECT_EQ(fx.optimizedCall("sum", "100"), "5050");
    EXPECT_EQ(fx.optimizedCall("mix", "1.5"), "259");
    EXPECT_EQ(fx.optimizedCall("bumpy", "10"), "20");
    EXPECT_TRUE(!Alka::Detail::kFastApiUsable || FastFixture::fastCalls() > before);
}

TEST(FastApi, FastPathValidatesAndThrowsExactlyLikeTheSlowPath) {
    FastFixture fx;
    fx.evalString("globalThis.tryAdd = x => { try { return fast.add(x, 1); } catch (e) { return "
                  "e.message; } };"
                  "globalThis.tryChecked = x => { try { return fast.checked(x); } catch (e) {"
                  " return (e instanceof Error) + ':' + e.message; } }; 'ok'");
    EXPECT_EQ(fx.optimizedCall("tryAdd", "2"), "3");
    EXPECT_EQ(fx.evalString("String(tryAdd(1.5))"),
              "arg 1 (a) of fast.add: expected integer, got non-integer number");
    EXPECT_EQ(fx.evalString("String(tryAdd(2 ** 40))"),
              "arg 1 (a) of fast.add: int32 cannot represent this number");
    EXPECT_EQ(fx.optimizedCall("tryChecked", "5"), "5");
    EXPECT_EQ(fx.evalString("String(tryChecked(-1))"), "true:negative input");
}

TEST(FastApi, OnTheFastPathTooTheNativeMethodUnderAJSOverrideRunsTheCxxBase) {
    FastFixture fx;
    fx.evalString("const nativeRead = fast.Meter.prototype.read;"
                  "class Loud extends fast.Meter { read(x) { return super.read(x) * 10; } }"
                  "globalThis.loud = new Loud();"
                  "globalThis.viaSuper = x => loud.read(x);"
                  "globalThis.viaNative = x => nativeRead.call(loud, x); 'ok'");
    EXPECT_EQ(fx.evalString("String(fast.readOf(loud, 2))"), "30");
    const auto before = FastFixture::fastCalls();
    EXPECT_EQ(fx.optimizedCall("viaSuper", "2"), "30");
    EXPECT_EQ(fx.optimizedCall("viaNative", "2"), "3");
    EXPECT_TRUE(!Alka::Detail::kFastApiUsable || FastFixture::fastCalls() > before);
}

TEST(FastApi, NoFastKeepsADeclarationOffTheFastPath) {
    FastFixture fx;
    fx.evalString("globalThis.plain = x => fast.slowOnly(x); 'ok'");
    const auto before = FastFixture::fastCalls();
    EXPECT_EQ(fx.optimizedCall("plain", "7"), "7");
    EXPECT_EQ(FastFixture::fastCalls(), before);
}

TEST(FastApi, StringsReachTheFastPathAsUTF8OtherStringsTakeTheRegularOne) {
    FastFixture fx;
    fx.evalString(
        "globalThis.len = s => fast.byteLength(s);"
        "globalThis.tryLen = s => { try { return fast.byteLength(s); } catch (e) { return "
        "e.message; } };"
        "globalThis.dots = s => fast.count(s, 46); globalThis.look = s => fast.see(s); 'ok'");
    EXPECT_EQ(fx.optimizedCall("len", "'abc'"), "3");
    EXPECT_EQ(fx.optimizedCall("dots", "'a.b.c'"), "2");
    EXPECT_EQ(fx.evalString("String(len('caf\u00e9 cr\u00e8me'))"), "12");
    EXPECT_EQ(fx.evalString("String(len('\u4f60\u597d'))"), "6");
    EXPECT_EQ(fx.evalString(
                  "String(len('ab'.repeat(20) + String(Math.random()).slice(0, 1).repeat(30)))"),
              "70"); // a rope
    EXPECT_EQ(fx.optimizedCall("tryLen", "'x'"), "1");
    const auto failed = fx.evalString("String(tryLen(42))");
    EXPECT_THAT(failed, testing::StartsWith("arg 1 (text) of fast.byteLength: "));
    EXPECT_THAT(failed, testing::HasSubstr("string"));

    const auto before = FastFixture::fastCalls();
    EXPECT_EQ(fx.optimizedCall("look", "'x'"), "undefined");
    EXPECT_TRUE(!Alka::Detail::kFastApiUsable || FastFixture::fastCalls() > before);
    const auto latin1 = FastFixture::fastCalls();
    fx.evalString("look('caf\\u00e9'); 'ok'");
    EXPECT_EQ(FastTest::gSeen, "caf\xC3\xA9");
    EXPECT_TRUE(!Alka::Detail::kFastApiUsable || FastFixture::fastCalls() > latin1);
    fx.evalString("look('\\u4f60'); 'ok'");
    EXPECT_EQ(FastTest::gSeen, "\xE4\xBD\xA0");
}
