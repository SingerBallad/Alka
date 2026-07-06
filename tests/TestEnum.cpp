#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <doctest/doctest.h>

namespace EnumTest {

enum class Color { Red, Green = 5, Blue };
enum Weekday { Monday = 1, Friday = 5 };
enum class Signed { Neg = -2, Zero = 0 };

inline Color swap(Color c) { return c == Color::Red ? Color::Blue : Color::Red; }

struct PaletteMod;

}  // namespace EnumTest

template<>
struct Alka::Bind<EnumTest::PaletteMod> : Alka::Module<"palette"> {
    static constexpr auto members = Alka::members(
        Alka::enumType<EnumTest::Color>(), Alka::enumType<EnumTest::Weekday>(),
        Alka::enumType<"Renamed", EnumTest::Signed>(),
        Alka::fn<&EnumTest::swap>(Alka::arg("c")));
};

namespace {

struct PaletteFixture : AlkaTest::V8Fixture {
    PaletteFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<EnumTest::PaletteMod>(context);
        });
    }
};

}  // namespace

TEST_CASE("enums install as TS-style two-way frozen objects") {
    PaletteFixture fx;
    CHECK(fx.evalInt("palette.Color.Red") == 0);
    CHECK(fx.evalInt("palette.Color.Green") == 5);
    CHECK(fx.evalInt("palette.Color.Blue") == 6);
    CHECK(fx.evalString("palette.Color[5]") == "Green");
    CHECK(fx.evalString("palette.Color[palette.Color.Blue]") == "Blue");
    CHECK(fx.evalBool("Object.isFrozen(palette.Color)"));
    CHECK(fx.evalBool("(() => { 'use strict'; try { palette.Color.Red = 9; return false; }"
                      " catch { return true; } })()"));
}

TEST_CASE("unscoped, renamed and negative enums work") {
    PaletteFixture fx;
    CHECK(fx.evalInt("palette.Weekday.Friday") == 5);
    CHECK(fx.evalString("palette.Weekday[1]") == "Monday");
    CHECK(fx.evalInt("palette.Renamed.Neg") == -2);
    CHECK(fx.evalString("palette.Renamed[-2]") == "Neg");
    CHECK(fx.evalString("typeof palette.Signed") == "undefined");
}

TEST_CASE("enum values convert through function boundaries") {
    PaletteFixture fx;
    CHECK(fx.evalInt("palette.swap(palette.Color.Red)") == 6);   // Blue
    CHECK(fx.evalInt("palette.swap(palette.Color.Blue)") == 0);  // Red
    CHECK(fx.evalString("try { palette.swap(42) } catch (e) { e.message }") ==
          "arg 1 (c) of palette.swap: 42 is not a valid Color value");
}
