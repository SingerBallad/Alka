#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace BenchTest {

struct Series {
    std::vector<double> mValues = std::vector<double>(50000, 1.0);
    [[nodiscard]] std::size_t size() const { return mValues.size(); }
    double& operator[](std::size_t i) { return mValues[i]; }
};

struct Rgb {
    double mR = 0;
    double mG = 0;
    double mB = 0;
};
inline Rgb brighten(Rgb c) {
    return Rgb{c.mR + 1, c.mG + 1, c.mB + 1};
}

struct Joint {
    double mX = 1;
};
struct Body {
    Joint mJoint;
};

struct Vec3 {
    double mX = 1;
    double mY = 2;
    double mZ = 3;
};

class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() { return 0; }
};
struct JsShape : Shape, Alka::Overridable<Shape> {
    double area() override { return ALKA_OVERRIDE(double, Shape, area); }
};
inline double sumAreas(std::shared_ptr<Shape> shape, int n) {
    double total = 0;
    for (int i = 0; i < n; ++i) {
        total += shape->area();
    }
    return total;
}

struct Mod;

} // namespace BenchTest

template <> struct Alka::Bind<BenchTest::Series> : Class<BenchTest::Series> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::iterable<BenchTest::Series>());
};

template <> struct Alka::Bind<BenchTest::Rgb> : Record<BenchTest::Rgb> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::prop<&BenchTest::Rgb::mR>(), Alka::prop<&BenchTest::Rgb::mG>(),
                      Alka::prop<&BenchTest::Rgb::mB>());
};

template <> struct Alka::Bind<BenchTest::Joint> : Class<BenchTest::Joint> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&BenchTest::Joint::mX>());
};

template <> struct Alka::Bind<BenchTest::Body> : Class<BenchTest::Body> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&BenchTest::Body::mJoint>());
};

template <> struct Alka::Bind<BenchTest::Vec3> : Class<BenchTest::Vec3> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::prop<&BenchTest::Vec3::mX>(), Alka::prop<&BenchTest::Vec3::mY>(),
        Alka::prop<&BenchTest::Vec3::mZ>(), Alka::toJSON<BenchTest::Vec3>());
};

template <> struct Alka::Bind<BenchTest::Shape> : Class<BenchTest::Shape> {
    using Trampoline = BenchTest::JsShape;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"area", &BenchTest::Shape::area>());
};

template <> struct Alka::Bind<BenchTest::Mod> : Module<"bench"> {
    static constexpr auto members = Alka::members(
        Alka::type<BenchTest::Series>, Alka::type<BenchTest::Rgb>, Alka::type<BenchTest::Joint>,
        Alka::type<BenchTest::Body>, Alka::type<BenchTest::Vec3>, Alka::type<BenchTest::Shape>,
        Alka::fn<&BenchTest::brighten>(), Alka::fn<&BenchTest::sumAreas>());
};

namespace {

struct BenchFixture : AlkaTest::V8Fixture {
    BenchFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<BenchTest::Mod>(context);
        });
    }

    void measure(const char* label, const char* source, const double calls, const double expect) {
        double best = 0;
        for (int run = 0; run < 5; ++run) {
            const auto start = std::chrono::steady_clock::now();
            const double result = evalDouble(source);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            EXPECT_EQ(result, expect);
            const auto ns = std::chrono::duration<double, std::nano>(elapsed).count();
            best = run == 0 || ns < best ? ns : best;
        }
        std::printf("  %-28s %9.1f ns/call\n", label, best / calls);
    }
};

} // namespace

TEST(Bench, DISABLED_HotPaths) {
    BenchFixture fx;
    std::printf("Alka hot paths:\n");
    fx.measure("iterator step (for...of)",
               "(() => { const s = new bench.Series(); let t = 0;"
               " for (let k = 0; k < 10; ++k) for (const v of s) t += v; return t; })()",
               500000, 500000);
    fx.measure("record in + out",
               "(() => { let c = { r: 0, g: 0, b: 0 };"
               " for (let i = 0; i < 100000; ++i) c = bench.brighten(c); return c.r; })()",
               100000, 100000);
    fx.measure("field view getter",
               "(() => { const b = new bench.Body(); let t = 0;"
               " for (let i = 0; i < 100000; ++i) t += b.joint.x; return t; })()",
               100000, 100000);
    fx.measure("toJSON",
               "(() => { const v = new bench.Vec3(); let t = 0;"
               " for (let i = 0; i < 100000; ++i) t += v.toJSON().z; return t; })()",
               100000, 300000);
    fx.measure("virtual call, not overridden",
               "bench.sumAreas(new (class extends bench.Shape {})(), 100000)", 100000, 0);
    fx.measure("override dispatch (C++->JS)",
               "bench.sumAreas(new (class extends bench.Shape { area() { return 2; } })(), 100000)",
               100000, 200000);
}
