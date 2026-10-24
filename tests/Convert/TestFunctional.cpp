#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <utility>

namespace FunctionalTest {

inline std::function<void(int)> gStoredCallback;

inline int apply(const std::function<int(int, int)>& f, int a, int b) {
    return f(a, b);
}
inline void setCallback(std::function<void(int)> cb) {
    gStoredCallback = std::move(cb);
}
inline void fire(int v) {
    if (gStoredCallback) {
        gStoredCallback(v);
    }
}
inline void clearCallback() {
    gStoredCallback = {};
}
inline std::function<int(int)> makeDoubler() {
    return [](int v) { return v * 2; };
}
inline int callTyped(const Alka::Function<int(int)>& f) {
    const auto result = f.tryCall(20);
    return result ? *result : -1;
}
inline Alka::Value passthrough(Alka::Value v) {
    return v;
}
inline int readX(const Alka::Object& o) {
    const auto x = o.getAs<int>("x");
    return x ? *x : -1;
}

struct CbMod;

} // namespace FunctionalTest

template <> struct Alka::Bind<FunctionalTest::CbMod> : Module<"cb"> {
    static constexpr auto members = Alka::members(
        Alka::fn<&FunctionalTest::apply>(arg("f"), arg("a"), arg("b")),
        Alka::fn<&FunctionalTest::setCallback>(arg("cb")), Alka::fn<&FunctionalTest::fire>(),
        Alka::fn<&FunctionalTest::clearCallback>(), Alka::fn<&FunctionalTest::makeDoubler>(),
        Alka::fn<&FunctionalTest::callTyped>(), Alka::fn<&FunctionalTest::passthrough>(),
        Alka::fn<&FunctionalTest::readX>());
};

namespace {

struct CbFixture : AlkaTest::V8Fixture {
    CbFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<FunctionalTest::CbMod>(context);
        });
    }
    ~CbFixture() { FunctionalTest::clearCallback(); }
};

} // namespace

TEST(Functional, JSFunctionsConvertToStdFunction) {
    CbFixture fx;
    EXPECT_EQ(fx.evalInt("cb.apply((a, b) => a * b, 6, 7)"), 42);
    EXPECT_EQ(fx.evalString("try { cb.apply(123, 1, 2) } catch (e) { e.message }"),
              "arg 1 (f) of cb.apply: expected function, got number");
}

TEST(Functional, StoredCallbacksFireLater) {
    CbFixture fx;
    fx.evalString("globalThis.log = []; cb.setCallback(v => log.push(v)); 'ok'");
    fx.evalString("cb.fire(1); cb.fire(2); 'ok'");
    EXPECT_EQ(fx.evalString("log.join(',')"), "1,2");
    FunctionalTest::clearCallback();
}

TEST(Functional, JSExceptionsInsideCallbacksCrossCxxUnchanged) {
    CbFixture fx;
    EXPECT_EQ(fx.evalString("try { cb.apply(() => { throw new Error('inner boom'); }, 1, 2) }"
                            " catch (e) { e.message }"),
              "inner boom");
    EXPECT_TRUE(
        fx.evalBool("(() => { class MyErr extends RangeError {}; const err = new MyErr('x');"
                    " err.code = 42;"
                    " try { cb.apply(() => { throw err; }, 1, 2); } catch (e) {"
                    " return e === err && e instanceof MyErr && e.code === 42; }"
                    " return false; })()"));
    EXPECT_EQ(fx.evalInt("try { cb.apply(() => { throw 7; }, 1, 2) } catch (e) { e }"), 7);
}

TEST(Functional, CallbackReturnValuesConvertBackAndMismatchesReport) {
    CbFixture fx;
    EXPECT_EQ(fx.evalString("try { cb.apply(() => 'nope', 1, 2) } catch (e) { e.message }"),
              "JS callback returned an incompatible value: expected number, got string");
}

TEST(Functional, CxxCallablesConvertToCallableJSFunctions) {
    CbFixture fx;
    EXPECT_EQ(fx.evalInt("cb.makeDoubler()(21)"), 42);
    EXPECT_EQ(fx.evalString("typeof cb.makeDoubler()"), "function");
}

TEST(Functional, TypedAlkaFunctionParameters) {
    CbFixture fx;
    EXPECT_EQ(fx.evalInt("cb.callTyped(v => v + 1)"), 21);
}

TEST(Functional, AlkaValuePassesAnyJSValueThroughWithIdentity) {
    CbFixture fx;
    EXPECT_TRUE(fx.evalBool("(() => { const o = { a: 1 }; return cb.passthrough(o) === o; })()"));
    EXPECT_TRUE(fx.evalBool("cb.passthrough(undefined) === undefined"));
    EXPECT_EQ(fx.evalInt("cb.passthrough(42)"), 42);
}

TEST(Functional, AlkaObjectKeyedAccess) {
    CbFixture fx;
    EXPECT_EQ(fx.evalInt("cb.readX({ x: 33 })"), 33);
    EXPECT_EQ(fx.evalInt("cb.readX({})"), -1);
    EXPECT_EQ(fx.evalString("try { cb.readX(5) } catch (e) { e.message }"),
              "arg 1 of cb.readX: expected object, got number");
}
