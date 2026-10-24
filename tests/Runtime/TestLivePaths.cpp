#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace PathTest {

struct Point {
    Point() = default;
    explicit Point(int x) : mX(x) {}
    int mX = 0;
};

struct Segment {
    Point mFrom;
    Point mTo;
};

struct Polygon {
    std::vector<Point> mPoints;
    void add(int x) { mPoints.emplace_back(x); }
    void clear() { mPoints.clear(); }
    [[nodiscard]] std::size_t size() const { return mPoints.size(); }
    Point& operator[](std::size_t i) { return mPoints[i]; }
};

struct Polyline {
    std::vector<Segment> mSegments;
    void add(int from, int to) { mSegments.push_back(Segment{Point(from), Point(to)}); }
    [[nodiscard]] std::size_t size() const { return mSegments.size(); }
    Segment& operator[](std::size_t i) { return mSegments[i]; }
};

class Animal {
public:
    virtual ~Animal() = default;
    [[nodiscard]] virtual std::string speak() const { return "..."; }
};
class Dog : public Animal {
public:
    explicit Dog(std::string name) : mName(std::move(name)) {}
    [[nodiscard]] std::string speak() const override { return mName + ": woof"; }
    std::string mName;
};
class Cat : public Animal {
public:
    [[nodiscard]] std::string speak() const override { return "meow"; }
};

struct Kennel {
    std::vector<Point> mSpots{Point(1)};
    std::vector<std::unique_ptr<Animal>> mAnimals;
    Point& first() { return mSpots.front(); }
    Point& spot(int i) { return mSpots.at(static_cast<std::size_t>(i)); }
    void grow(int n) {
        for (int i = 0; i < n; ++i) {
            mSpots.emplace_back(100 + i);
        }
    }
    void shrink() { mSpots.resize(1); }
    Animal* animal(int i) {
        return i < static_cast<int>(mAnimals.size()) ? mAnimals[static_cast<std::size_t>(i)].get()
                                                     : nullptr;
    }
    void putDog(int i, std::string name) { put(i, std::make_unique<Dog>(std::move(name))); }
    void putCat(int i) { put(i, std::make_unique<Cat>()); }

private:
    void put(int i, std::unique_ptr<Animal> a) {
        const auto at = static_cast<std::size_t>(i);
        if (mAnimals.size() <= at) {
            mAnimals.resize(at + 1);
        }
        mAnimals[at] = std::move(a);
    }
};

struct Mod;

} // namespace PathTest

template <> struct Alka::Bind<PathTest::Point> : Class<PathTest::Point> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(), Alka::prop<&PathTest::Point::mX>());
};

template <> struct Alka::Bind<PathTest::Segment> : Class<PathTest::Segment> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&PathTest::Segment::mFrom>(),
                                                  Alka::prop<&PathTest::Segment::mTo>());
};

template <> struct Alka::Bind<PathTest::Polygon> : Class<PathTest::Polygon> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&PathTest::Polygon::add>(),
                      Alka::method<&PathTest::Polygon::clear>(), Alka::indexer(policy::internalRef),
                      Alka::iterable<PathTest::Polygon>(policy::internalRef));
};

template <> struct Alka::Bind<PathTest::Polyline> : Class<PathTest::Polyline> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&PathTest::Polyline::add>(), indexer(policy::internalRef));
};

template <> struct Alka::Bind<PathTest::Animal> : Class<PathTest::Animal> {
    static constexpr auto members =
        Alka::members(Alka::method<"speak", &PathTest::Animal::speak>());
};
template <> struct Alka::Bind<PathTest::Dog> : Class<PathTest::Dog> {
    static constexpr auto naming = memberPrefixes;
    using Extends = PathTest::Animal;
    static constexpr auto members = Alka::members(Alka::prop<&PathTest::Dog::mName>());
};
template <> struct Alka::Bind<PathTest::Cat> : Class<PathTest::Cat> {
    using Extends = PathTest::Animal;
    static constexpr auto members = Alka::members();
};

template <> struct Alka::Bind<PathTest::Kennel> : Class<PathTest::Kennel> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&PathTest::Kennel::first>(policy::internalRef),
        Alka::method<&PathTest::Kennel::spot>(policy::internalRef),
        Alka::method<&PathTest::Kennel::animal>(policy::internalRef),
        Alka::method<&PathTest::Kennel::grow>(), Alka::method<&PathTest::Kennel::shrink>(),
        Alka::method<&PathTest::Kennel::putDog>(), Alka::method<&PathTest::Kennel::putCat>());
};

template <> struct Alka::Bind<PathTest::Mod> : Module<"paths"> {
    static constexpr auto members = Alka::members(
        Alka::type<PathTest::Point>, Alka::type<PathTest::Segment>, Alka::type<PathTest::Polygon>,
        Alka::type<PathTest::Polyline>, Alka::type<PathTest::Animal>, Alka::type<PathTest::Dog>,
        Alka::type<PathTest::Cat>, Alka::type<PathTest::Kennel>);
};

namespace {

struct PathFixture : AlkaTest::V8Fixture {
    PathFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<PathTest::Mod>(context);
        });
    }
};

} // namespace

TEST(LivePaths, ElementViewsSurviveTheirContainerReallocating) {
    PathFixture fx;
    fx.evalString("globalThis.p = new paths.Polygon(); p.add(1); globalThis.first = p[0];"
                  "for (let i = 0; i < 1000; ++i) p.add(i); 'ok'");
    EXPECT_EQ(fx.evalInt("first.x"), 1);
    fx.evalString("first.x = 5; 'ok'");
    EXPECT_EQ(fx.evalInt("p[0].x"), 5);
}

TEST(LivePaths, ElementViewWhoseElementIsGoneThrowsInsteadOfDangling) {
    PathFixture fx;
    fx.evalString("globalThis.p = new paths.Polygon(); p.add(1); p.add(2);"
                  "globalThis.second = p[1]; p.clear(); 'ok'");
    EXPECT_EQ(fx.evalString("try { second.x } catch (e) { e.message }"),
              "Point.x: the native Point object is not (or no longer) alive (element 1 is gone: "
              "the container holds 0)");
    fx.evalString("p.add(7); p.add(8); 'ok'");
    EXPECT_EQ(fx.evalInt("second.x"), 8);
}

TEST(LivePaths, ElementViewIsOneObjectPerSlotAlsoWhenIterated) {
    PathFixture fx;
    fx.evalString("globalThis.p = new paths.Polygon(); p.add(1); p.add(2); 'ok'");
    EXPECT_TRUE(fx.evalBool("p[0] === p[0] && p[1] !== p[0]"));
    EXPECT_TRUE(fx.evalBool("[...p][1] === p[1]"));
    fx.evalString("p[0].tag = 'kept'; 'ok'");
    EXPECT_EQ(fx.evalString("p[0].tag"), "kept");
}

TEST(LivePaths, FieldOfAnElementViewFollowsTheElement) {
    PathFixture fx;
    fx.evalString("globalThis.l = new paths.Polyline(); l.add(1, 2); globalThis.to = l[0].to;"
                  "for (let i = 0; i < 1000; ++i) l.add(i, i); 'ok'");
    EXPECT_EQ(fx.evalInt("to.x"), 2);
    fx.evalString("to.x = 20; 'ok'");
    EXPECT_EQ(fx.evalInt("l[0].to.x"), 20);
    EXPECT_TRUE(fx.evalBool("l[0].to === l[0].to"));
}

TEST(LivePaths, InternalRefAccessorResultRunsTheAccessorAgainOnEveryUse) {
    PathFixture fx;
    fx.evalString("globalThis.k = new paths.Kennel(); globalThis.f = k.first();"
                  "globalThis.s = k.spot(0); k.grow(1000); 'ok'");
    EXPECT_EQ(fx.evalInt("f.x"), 1);
    EXPECT_EQ(fx.evalInt("s.x"), 1);
    EXPECT_TRUE(fx.evalBool("k.first() === f"));
    fx.evalString("globalThis.far = k.spot(500); k.shrink(); 'ok'");
    EXPECT_TRUE(fx.evalBool(
        "(() => { try { far.x; return false; } catch (e) { return e.message"
        ".startsWith('Point.x: the native Point object is not (or no longer) alive ('); }"
        " })()"));
    EXPECT_EQ(fx.evalInt("f.x"), 1);
}

TEST(LivePaths, LivePathYieldsNothingOnceAnotherTypeOfObjectIsThere) {
    PathFixture fx;
    fx.evalString(
        "globalThis.k = new paths.Kennel(); k.putDog(0, 'rex'); globalThis.a = k.animal(0);"
        "'ok'");
    EXPECT_TRUE(fx.evalBool("a instanceof paths.Dog"));
    EXPECT_EQ(fx.evalString("a.speak()"), "rex: woof");
    fx.evalString("k.putDog(0, 'max'); 'ok'");
    EXPECT_EQ(fx.evalString("a.speak()"), "max: woof");
    fx.evalString("k.putCat(0); 'ok'");
    EXPECT_EQ(fx.evalString("try { a.speak() } catch (e) { e.message }"),
              "Animal.speak: the native Animal object is not (or no longer) alive (another type of "
              "object is there now)");
}
