#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <utility>

namespace FunctionalTest {

inline std::function<void(int)> gStoredCallback;

inline int apply(const std::function<int(int, int)>& f, int a, int b) { return f(a, b); }
inline void setCallback(std::function<void(int)> cb) { gStoredCallback = std::move(cb); }
inline void fire(int v) {
    if (gStoredCallback) {
        gStoredCallback(v);
    }
}
inline void clearCallback() { gStoredCallback = {}; }
inline std::function<int(int)> makeDoubler() {
    return [](int v) { return v * 2; };
}
inline int callTyped(const Alka::Function<int(int)>& f) {
    auto result = f.tryCall(20);
    return result ? *result : -1;
}
inline Alka::Value passthrough(Alka::Value v) { return v; }
inline int readX(const Alka::Object& o) {
    auto x = o.getAs<int>("x");
    return x ? *x : -1;
}

struct CbMod;

}  // namespace FunctionalTest

template<>
struct Alka::Bind<FunctionalTest::CbMod> : Alka::Module<"cb"> {
    static constexpr auto members = Alka::members(
        Alka::fn<&FunctionalTest::apply>(Alka::arg("f"), Alka::arg("a"), Alka::arg("b")),
        Alka::fn<&FunctionalTest::setCallback>(Alka::arg("cb")),
        Alka::fn<&FunctionalTest::fire>(), Alka::fn<&FunctionalTest::clearCallback>(),
        Alka::fn<&FunctionalTest::makeDoubler>(), Alka::fn<&FunctionalTest::callTyped>(),
        Alka::fn<&FunctionalTest::passthrough>(), Alka::fn<&FunctionalTest::readX>());
};

namespace {

struct CbFixture : AlkaTest::V8Fixture {
    CbFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<FunctionalTest::CbMod>(context);
        });
    }
    ~CbFixture() { FunctionalTest::clearCallback(); }  // Globals must die before the isolate
};

}  // namespace

TEST_CASE("JS functions convert to std::function") {
    CbFixture fx;
    CHECK(fx.evalInt("cb.apply((a, b) => a * b, 6, 7)") == 42);
    CHECK(fx.evalString("try { cb.apply(123, 1, 2) } catch (e) { e.message }") ==
          "arg 1 (f) of cb.apply: expected function, got number");
}

TEST_CASE("stored callbacks fire later") {
    CbFixture fx;
    fx.evalString("globalThis.log = []; cb.setCallback(v => log.push(v)); 'ok'");
    fx.evalString("cb.fire(1); cb.fire(2); 'ok'");
    CHECK(fx.evalString("log.join(',')") == "1,2");
    FunctionalTest::clearCallback();
}

TEST_CASE("JS exceptions inside callbacks surface as JS errors at the outer boundary") {
    CbFixture fx;
    CHECK(fx.evalString(
              "try { cb.apply(() => { throw new Error('inner boom'); }, 1, 2) }"
              " catch (e) { e.message.split('\\n')[0] }") == "Error: inner boom");
}

TEST_CASE("callback return values convert back (and mismatches report)") {
    CbFixture fx;
    CHECK(fx.evalString("try { cb.apply(() => 'nope', 1, 2) } catch (e) { e.message }") ==
          "JS callback returned an incompatible value: expected number, got string");
}

TEST_CASE("C++ callables convert to callable JS functions") {
    CbFixture fx;
    CHECK(fx.evalInt("cb.makeDoubler()(21)") == 42);
    CHECK(fx.evalString("typeof cb.makeDoubler()") == "function");
}

TEST_CASE("typed Alka::Function parameters") {
    CbFixture fx;
    CHECK(fx.evalInt("cb.callTyped(v => v + 1)") == 21);
}

TEST_CASE("Alka::Value passes any JS value through with identity") {
    CbFixture fx;
    CHECK(fx.evalBool("(() => { const o = { a: 1 }; return cb.passthrough(o) === o; })()"));
    CHECK(fx.evalBool("cb.passthrough(undefined) === undefined"));
    CHECK(fx.evalInt("cb.passthrough(42)") == 42);
}

TEST_CASE("Alka::Object keyed access") {
    CbFixture fx;
    CHECK(fx.evalInt("cb.readX({ x: 33 })") == 33);
    CHECK(fx.evalInt("cb.readX({})") == -1);
    CHECK(fx.evalString("try { cb.readX(5) } catch (e) { e.message }") ==
          "arg 1 of cb.readX: expected object, got number");
}
