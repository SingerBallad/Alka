#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <string_view>

TEST_CASE("v8 boots and evaluates javascript") {
    AlkaTest::V8Fixture fx;
    CHECK(fx.evalInt("1 + 1") == 2);
    CHECK(fx.evalString("['al', 'ka'].join('')") == "alka");
    CHECK(fx.evalBool("typeof Promise === 'function'"));
}

TEST_CASE("icu data is loaded (Intl works)") {
    AlkaTest::V8Fixture fx;
    CHECK(fx.evalBool("typeof Intl === 'object'"));
    CHECK(fx.evalString("new Intl.NumberFormat('de-DE').format(12345.6)") == "12.345,6");
}

TEST_CASE("alka reports its version") {
    CHECK(std::string_view(Alka::versionString()) == ALKA_VERSION_STRING);
}
