#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <string_view>

TEST(Smoke, V8BootsAndEvaluatesJavascript) {
    AlkaTest::V8Fixture fx;
    EXPECT_EQ(fx.evalInt("1 + 1"), 2);
    EXPECT_EQ(fx.evalString("['al', 'ka'].join('')"), "alka");
    EXPECT_TRUE(fx.evalBool("typeof Promise === 'function'"));
}

TEST(Smoke, IcuDataIsLoadedIntlWorks) {
    AlkaTest::V8Fixture fx;
    EXPECT_TRUE(fx.evalBool("typeof Intl === 'object'"));
    EXPECT_EQ(fx.evalString("new Intl.NumberFormat('de-DE').format(12345.6)"), "12.345,6");
}

TEST(Smoke, AlkaReportsItsVersion) {
    EXPECT_EQ(std::string_view(Alka::versionString()), ALKA_VERSION_STRING);
}
