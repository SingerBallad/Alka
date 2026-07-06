#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

namespace FnTest {

inline int add(int a, int b) { return a + b; }
inline std::string greet(const std::string& name) { return "hi " + name; }
inline void tick(double) {}
inline int boomInside() { throw std::runtime_error("boom from C++"); }

// overload set material
inline std::string pick(int) { return "int"; }
inline std::string pick(const std::string&) { return "string"; }
inline std::string pick(int, int) { return "two ints"; }
inline std::string pickNum(double) { return "double"; }
inline std::string pickNum(bool) { return "bool"; }

// defaults
inline std::string greetDef(const std::string& name, const std::string& greeting) {
    return greeting + " " + name;
}
inline double scaleDef(double value, double factor, double bias) {
    return value * factor + bias;
}

struct MathMod;
struct ExtraMod;

}  // namespace FnTest

template<>
struct Alka::Bind<FnTest::ExtraMod> : Alka::Module<"math.extra"> {
    static constexpr auto members =
        Alka::members(Alka::fn<"neg", [](int v) { return -v; }>());
};

template<>
struct Alka::Bind<FnTest::MathMod> : Alka::Module<"math"> {
    static constexpr auto doc = "math utilities";
    static constexpr auto members = Alka::members(
        Alka::fn<&FnTest::add>(Alka::arg("a"), Alka::arg("b")),
        Alka::fn<"plus", &FnTest::add>(),
        Alka::fn<&FnTest::greet>(Alka::arg("name")),
        Alka::fn<&FnTest::tick>(),
        Alka::fn<"boom", &FnTest::boomInside>(),
        Alka::fn<"mul", [](int a, int b) { return a * b; }>(),
        // overload set: first declared match wins
        Alka::fn<"pick", static_cast<std::string (*)(int)>(&FnTest::pick),
                 static_cast<std::string (*)(const std::string&)>(&FnTest::pick),
                 static_cast<std::string (*)(int, int)>(&FnTest::pick)>(),
        Alka::fn<"pickNum", static_cast<std::string (*)(double)>(&FnTest::pickNum),
                 static_cast<std::string (*)(bool)>(&FnTest::pickNum)>(),
        // defaults fill missing/undefined arguments
        Alka::fn<&FnTest::greetDef>(Alka::arg("name"), Alka::arg("greeting") = "hi"),
        Alka::fn<&FnTest::scaleDef>(Alka::arg("value"), Alka::arg("factor") = 2.0,
                                    Alka::arg("bias") = 1.0),
        Alka::var<"VERSION">(3),
        Alka::var<"NAME">("alka"),
        Alka::submodule<FnTest::ExtraMod>);
};

namespace {

struct InstalledFixture : AlkaTest::V8Fixture {
    InstalledFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<FnTest::MathMod>(context);
        });
    }
};

}  // namespace

TEST_CASE("free functions are callable from JS") {
    InstalledFixture fx;
    CHECK(fx.evalInt("math.add(2, 3)") == 5);
    CHECK(fx.evalInt("math.plus(10, 5)") == 15);
    CHECK(fx.evalString("math.greet('bob')") == "hi bob");
    CHECK(fx.evalInt("math.mul(6, 7)") == 42);
    CHECK(fx.evalString("typeof math.tick(1.5)") == "undefined");
    CHECK(fx.evalString("typeof math.add") == "function");
}

TEST_CASE("vars install as readonly constants") {
    InstalledFixture fx;
    CHECK(fx.evalInt("math.VERSION") == 3);
    CHECK(fx.evalString("math.NAME") == "alka");
    CHECK(fx.evalBool("(() => { 'use strict'; try { math.VERSION = 9; return false; }"
                      " catch { return true; } })()"));
    CHECK(fx.evalInt("(() => { math.VERSION; return math.VERSION; })()") == 3);
}

TEST_CASE("submodules install under their dotted path") {
    InstalledFixture fx;
    CHECK(fx.evalInt("math.extra.neg(5)") == -5);
    CHECK(fx.evalString("typeof math.extra") == "object");
}

TEST_CASE("conversion failures throw TypeError with argument context") {
    InstalledFixture fx;
    CHECK(fx.evalString("try { math.add(1, 'x') } catch (e) { e.message }") ==
          "arg 2 (b) of math.add: expected number, got string");
    CHECK(fx.evalString("try { math.add(1) } catch (e) { e.message }") ==
          "arg 2 (b) of math.add: expected number, got undefined");
    CHECK(fx.evalBool("(() => { try { math.add(1, 'x'); return false; }"
                      " catch (e) { return e instanceof TypeError; } })()"));
    // no arg() spec on mul: message falls back to the bare index
    CHECK(fx.evalString("try { math.mul('x', 1) } catch (e) { e.message }") ==
          "arg 1 of math.mul: expected number, got string");
}

TEST_CASE("C++ exceptions surface as JS errors") {
    InstalledFixture fx;
    CHECK(fx.evalString("try { math.boom() } catch (e) { e.message }") == "boom from C++");
    CHECK(fx.evalBool("(() => { try { math.boom(); return false; }"
                      " catch (e) { return e instanceof Error; } })()"));
}

TEST_CASE("extra JS arguments are ignored") {
    InstalledFixture fx;
    CHECK(fx.evalInt("math.add(1, 2, 'junk', null)") == 3);
}

TEST_CASE("overload sets dispatch by arity and argument types") {
    InstalledFixture fx;
    CHECK(fx.evalString("math.pick(1)") == "int");
    CHECK(fx.evalString("math.pick('x')") == "string");
    CHECK(fx.evalString("math.pick(1, 2)") == "two ints");
    // ambiguity resolves to the first declared overload
    CHECK(fx.evalString("math.pickNum(3)") == "double");
    CHECK(fx.evalString("math.pickNum(true)") == "bool");
    CHECK(fx.evalString("try { math.pick(true) } catch (e) { e.message }") ==
          "no overload of math.pick matches the given arguments (boolean)");
    CHECK(fx.evalString("try { math.pick(1, 2, 3) } catch (e) { e.message }") ==
          "no overload of math.pick matches the given arguments (number, number, number)");
}

TEST_CASE("arg() defaults fill missing or undefined arguments") {
    InstalledFixture fx;
    CHECK(fx.evalString("math.greetDef('bob')") == "hi bob");
    CHECK(fx.evalString("math.greetDef('bob', 'yo')") == "yo bob");
    CHECK(fx.evalString("math.greetDef('bob', undefined)") == "hi bob");
    CHECK(fx.evalDouble("math.scaleDef(10)") == 21.0);
    CHECK(fx.evalDouble("math.scaleDef(10, 3)") == 31.0);
    CHECK(fx.evalDouble("math.scaleDef(10, 3, 0)") == 30.0);
    // a middle undefined picks its default while later args stay explicit
    CHECK(fx.evalDouble("math.scaleDef(10, undefined, 0)") == 20.0);
    // missing non-defaulted args still error
    CHECK(fx.evalString("try { math.greetDef() } catch (e) { e.message }") ==
          "arg 1 (name) of math.greetDef: expected string, got undefined");
}
