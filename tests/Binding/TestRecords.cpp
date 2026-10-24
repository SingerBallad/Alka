#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace RecordTest {

struct Vec2 {
    double mX = 0;
    double mY = 0;
};

struct Rect {
    Vec2 mMin;
    Vec2 mMax;
    std::optional<std::string> mLabel;
    std::string mId;
};

inline double area(const Rect& r) {
    return (r.mMax.mX - r.mMin.mX) * (r.mMax.mY - r.mMin.mY);
}
inline Rect makeRect(double w, double h) {
    return Rect{{0, 0}, {w, h}, std::nullopt, "r"};
}
inline Vec2 sum(const std::vector<Vec2>& points) {
    Vec2 out;
    for (const auto& p : points) {
        out.mX += p.mX;
        out.mY += p.mY;
    }
    return out;
}

struct Mod;

} // namespace RecordTest

template <> struct Alka::Bind<RecordTest::Vec2> : Record<RecordTest::Vec2> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto doc = "a 2D point";
    static constexpr auto members =
        Alka::members(Alka::prop<&RecordTest::Vec2::mX>(), Alka::prop<&RecordTest::Vec2::mY>());
};

template <> struct Alka::Bind<RecordTest::Rect> : Record<RecordTest::Rect> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::prop<&RecordTest::Rect::mMin>(), Alka::prop<&RecordTest::Rect::mMax>(),
                      Alka::prop<&RecordTest::Rect::mLabel>(doc("optional caption")),
                      Alka::prop<&RecordTest::Rect::mId>(readonly));
};

template <> struct Alka::Bind<RecordTest::Mod> : Module<"geo"> {
    static constexpr auto members =
        Alka::members(Alka::type<RecordTest::Vec2>, Alka::type<RecordTest::Rect>,
                      Alka::fn<&RecordTest::area>(arg("r")), Alka::fn<&RecordTest::makeRect>(),
                      Alka::fn<&RecordTest::sum>(arg("points")));
};

namespace {

struct RecordFixture : AlkaTest::V8Fixture {
    RecordFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<RecordTest::Mod>(context);
        });
    }
};

} // namespace

TEST(Records, ConvertFromAndToPlainObjects) {
    RecordFixture fx;
    EXPECT_EQ(fx.evalDouble("geo.area({ min: { x: 0, y: 0 }, max: { x: 2, y: 3 }, id: 'a' })"),
              6.0);
    EXPECT_EQ(fx.evalString("JSON.stringify(geo.makeRect(2, 3))"),
              R"({"min":{"x":0,"y":0},"max":{"x":2,"y":3},"id":"r"})");
    EXPECT_EQ(fx.evalString("JSON.stringify(geo.sum([{ x: 1, y: 2 }, { x: 3, y: 4 }]))"),
              R"({"x":4,"y":6})");
    EXPECT_TRUE(fx.evalBool("Object.getPrototypeOf(geo.makeRect(1, 1)) === Object.prototype"));
    EXPECT_TRUE(fx.evalBool("typeof geo.Vec2 === 'undefined'"));
}

TEST(Records, RecordConversionErrorsNameTheOffendingKey) {
    RecordFixture fx;
    EXPECT_EQ(fx.evalString("try { geo.area({ min: { x: 0 }, max: { x: 1, y: 1 }, id: 'a' }) }"
                            " catch (e) { e.message }"),
              "arg 1 (r) of geo.area: key 'min': key 'y': expected number, got undefined");
    EXPECT_EQ(fx.evalString("try { geo.area(5) } catch (e) { e.message }"),
              "arg 1 (r) of geo.area: expected object, got number");
    EXPECT_EQ(fx.evalDouble(
                  "geo.area({ min: { x: 0, y: 0 }, max: { x: 1, y: 1 }, id: 'a', label: 'x' })"),
              1.0);
}

TEST(Records, EmitTypeScriptInterfaces) {
    const auto dts = Alka::generateDts<RecordTest::Mod>();
    EXPECT_THAT(
        dts, testing::HasSubstr("interface Vec2 {\n        x: number;\n        y: number;\n    }"));
    EXPECT_THAT(dts, testing::HasSubstr("label?: string;"));
    EXPECT_THAT(dts, testing::HasSubstr("readonly id: string;"));
    EXPECT_THAT(dts, testing::HasSubstr("function area(r: geo.Rect): number;"));
    EXPECT_THAT(dts, testing::HasSubstr("function sum(points: geo.Vec2[]): geo.Vec2;"));
    EXPECT_THAT(dts, testing::Not(testing::HasSubstr("class Vec2")));
}
