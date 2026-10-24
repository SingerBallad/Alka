#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

namespace EnumTest {

enum class Color { Red, Green = 5, Blue };
enum Weekday { Monday = 1, Friday = 5 };
enum class Signed { Neg = -2, Zero = 0 };

inline Color swap(Color c) {
    return c == Color::Red ? Color::Blue : Color::Red;
}

struct PaletteMod;

} // namespace EnumTest

template <> struct Alka::Bind<EnumTest::PaletteMod> : Alka::Module<"palette"> {
    static constexpr auto members = Alka::members(
        Alka::enumType<EnumTest::Color>(), Alka::enumType<EnumTest::Weekday>(),
        Alka::enumType<"Renamed", EnumTest::Signed>(), Alka::fn<&EnumTest::swap>(Alka::arg("c")));
};

namespace {

struct PaletteFixture : AlkaTest::V8Fixture {
    PaletteFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<EnumTest::PaletteMod>(context);
        });
    }
};

} // namespace

TEST(Enum, EnumsInstallAsTSStyleTwoWayFrozenObjects) {
    PaletteFixture fx;
    EXPECT_EQ(fx.evalInt("palette.Color.Red"), 0);
    EXPECT_EQ(fx.evalInt("palette.Color.Green"), 5);
    EXPECT_EQ(fx.evalInt("palette.Color.Blue"), 6);
    EXPECT_EQ(fx.evalString("palette.Color[5]"), "Green");
    EXPECT_EQ(fx.evalString("palette.Color[palette.Color.Blue]"), "Blue");
    EXPECT_TRUE(fx.evalBool("Object.isFrozen(palette.Color)"));
    EXPECT_TRUE(fx.evalBool("(() => { 'use strict'; try { palette.Color.Red = 9; return false; }"
                            " catch { return true; } })()"));
}

TEST(Enum, UnscopedRenamedAndNegativeEnumsWork) {
    PaletteFixture fx;
    EXPECT_EQ(fx.evalInt("palette.Weekday.Friday"), 5);
    EXPECT_EQ(fx.evalString("palette.Weekday[1]"), "Monday");
    EXPECT_EQ(fx.evalInt("palette.Renamed.Neg"), -2);
    EXPECT_EQ(fx.evalString("palette.Renamed[-2]"), "Neg");
    EXPECT_EQ(fx.evalString("typeof palette.Signed"), "undefined");
}

TEST(Enum, ValuesConvertThroughFunctionBoundaries) {
    PaletteFixture fx;
    EXPECT_EQ(fx.evalInt("palette.swap(palette.Color.Red)"), 6);  // Blue
    EXPECT_EQ(fx.evalInt("palette.swap(palette.Color.Blue)"), 0); // Red
    EXPECT_EQ(fx.evalString("try { palette.swap(42) } catch (e) { e.message }"),
              "arg 1 (c) of palette.swap: 42 is not a valid Color value");
}
