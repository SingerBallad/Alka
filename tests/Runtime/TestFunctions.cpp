#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace FnTest {

inline int add(int a, int b) {
    return a + b;
}
inline std::string greet(const std::string& name) {
    return "hi " + name;
}
inline void tick(double) {}
inline int boomInside() {
    throw std::runtime_error("boom from C++");
}

inline std::string pick(int) {
    return "int";
}
inline std::string pick(const std::string&) {
    return "string";
}
inline std::string pick(int, int) {
    return "two ints";
}
inline std::string pickNum(double) {
    return "double";
}
inline std::string pickNum(bool) {
    return "bool";
}

inline std::string greetDef(const std::string& name, const std::string& greeting) {
    return greeting + " " + name;
}
inline double scaleDef(double value, double factor, double bias) {
    return value * factor + bias;
}

struct MathMod;
struct ExtraMod;

} // namespace FnTest

template <> struct Alka::Bind<FnTest::ExtraMod> : Module<"math.extra"> {
    static constexpr auto members = Alka::members(Alka::fn<"neg", [](int v) { return -v; }>());
};

template <> struct Alka::Bind<FnTest::MathMod> : Module<"math"> {
    static constexpr auto doc = "math utilities";
    static constexpr auto members = Alka::members(
        Alka::fn<&FnTest::add>(arg("a"), arg("b")), Alka::fn<"plus", &FnTest::add>(),
        Alka::fn<&FnTest::greet>(arg("name")), Alka::fn<&FnTest::tick>(),
        Alka::fn<"boom", &FnTest::boomInside>(),
        Alka::fn<"mul", [](int a, int b) { return a * b; }>(),
        Alka::fn<"pick", static_cast<std::string (*)(int)>(&FnTest::pick),
                 static_cast<std::string (*)(const std::string&)>(&FnTest::pick),
                 static_cast<std::string (*)(int, int)>(&FnTest::pick)>(),
        Alka::fn<"pickNum", static_cast<std::string (*)(double)>(&FnTest::pickNum),
                 static_cast<std::string (*)(bool)>(&FnTest::pickNum)>(),
        Alka::fn<&FnTest::greetDef>(arg("name"), arg("greeting") = "hi"),
        Alka::fn<&FnTest::scaleDef>(arg("value"), arg("factor") = 2.0, arg("bias") = 1.0),
        Alka::var<"VERSION">(3), Alka::var<"NAME">("alka"), Alka::submodule<FnTest::ExtraMod>);
};

namespace {

struct InstalledFixture : AlkaTest::V8Fixture {
    InstalledFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<FnTest::MathMod>(context);
        });
    }
};

} // namespace

TEST(Functions, FreeFunctionsAreCallableFromJS) {
    InstalledFixture fx;
    EXPECT_EQ(fx.evalInt("math.add(2, 3)"), 5);
    EXPECT_EQ(fx.evalInt("math.plus(10, 5)"), 15);
    EXPECT_EQ(fx.evalString("math.greet('bob')"), "hi bob");
    EXPECT_EQ(fx.evalInt("math.mul(6, 7)"), 42);
    EXPECT_EQ(fx.evalString("typeof math.tick(1.5)"), "undefined");
    EXPECT_EQ(fx.evalString("typeof math.add"), "function");
}

TEST(Functions, VarsInstallAsReadonlyConstants) {
    InstalledFixture fx;
    EXPECT_EQ(fx.evalInt("math.VERSION"), 3);
    EXPECT_EQ(fx.evalString("math.NAME"), "alka");
    EXPECT_TRUE(fx.evalBool("(() => { 'use strict'; try { math.VERSION = 9; return false; }"
                            " catch { return true; } })()"));
    EXPECT_EQ(fx.evalInt("(() => { math.VERSION; return math.VERSION; })()"), 3);
}

TEST(Functions, SubmodulesInstallUnderTheirDottedPath) {
    InstalledFixture fx;
    EXPECT_EQ(fx.evalInt("math.extra.neg(5)"), -5);
    EXPECT_EQ(fx.evalString("typeof math.extra"), "object");
}

TEST(Functions, ConversionFailuresThrowTypeErrorWithArgumentContext) {
    InstalledFixture fx;
    EXPECT_EQ(fx.evalString("try { math.add(1, 'x') } catch (e) { e.message }"),
              "arg 2 (b) of math.add: expected number, got string");
    EXPECT_EQ(fx.evalString("try { math.add(1) } catch (e) { e.message }"),
              "arg 2 (b) of math.add: expected number, got undefined");
    EXPECT_TRUE(fx.evalBool("(() => { try { math.add(1, 'x'); return false; }"
                            " catch (e) { return e instanceof TypeError; } })()"));
    EXPECT_EQ(fx.evalString("try { math.mul('x', 1) } catch (e) { e.message }"),
              "arg 1 of math.mul: expected number, got string");
}

TEST(Functions, CxxExceptionsSurfaceAsJSErrors) {
    InstalledFixture fx;
    EXPECT_EQ(fx.evalString("try { math.boom() } catch (e) { e.message }"), "boom from C++");
    EXPECT_TRUE(fx.evalBool("(() => { try { math.boom(); return false; }"
                            " catch (e) { return e instanceof Error; } })()"));
}

TEST(Functions, ExtraJSArgumentsAreIgnored) {
    InstalledFixture fx;
    EXPECT_EQ(fx.evalInt("math.add(1, 2, 'junk', null)"), 3);
}

TEST(Functions, OverloadSetsDispatchByArityAndArgumentTypes) {
    InstalledFixture fx;
    EXPECT_EQ(fx.evalString("math.pick(1)"), "int");
    EXPECT_EQ(fx.evalString("math.pick('x')"), "string");
    EXPECT_EQ(fx.evalString("math.pick(1, 2)"), "two ints");
    EXPECT_EQ(fx.evalString("math.pickNum(3)"), "double");
    EXPECT_EQ(fx.evalString("math.pickNum(true)"), "bool");
    EXPECT_EQ(fx.evalString("try { math.pick(true) } catch (e) { e.message }"),
              "no overload of math.pick matches the given arguments (boolean)");
    EXPECT_EQ(fx.evalString("try { math.pick(1, 2, 3) } catch (e) { e.message }"),
              "no overload of math.pick matches the given arguments (number, number, number)");
}

TEST(Functions, ArgDefaultsFillMissingOrUndefinedArguments) {
    InstalledFixture fx;
    EXPECT_EQ(fx.evalString("math.greetDef('bob')"), "hi bob");
    EXPECT_EQ(fx.evalString("math.greetDef('bob', 'yo')"), "yo bob");
    EXPECT_EQ(fx.evalString("math.greetDef('bob', undefined)"), "hi bob");
    EXPECT_EQ(fx.evalDouble("math.scaleDef(10)"), 21.0);
    EXPECT_EQ(fx.evalDouble("math.scaleDef(10, 3)"), 31.0);
    EXPECT_EQ(fx.evalDouble("math.scaleDef(10, 3, 0)"), 30.0);
    EXPECT_EQ(fx.evalDouble("math.scaleDef(10, undefined, 0)"), 20.0);
    EXPECT_EQ(fx.evalString("try { math.greetDef() } catch (e) { e.message }"),
              "arg 1 (name) of math.greetDef: expected string, got undefined");
}
