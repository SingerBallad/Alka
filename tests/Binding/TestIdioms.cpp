#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace IdiomTest {

struct Numbers {
    std::vector<int> mValues;
    void add(int v) { mValues.push_back(v); }
    [[nodiscard]] std::size_t size() const { return mValues.size(); }
    int& operator[](std::size_t i) { return mValues[i]; }
};

struct MoreNumbers : Numbers {};

class Grid {
public:
    [[nodiscard]] int cell(std::size_t i) const { return mCells[i]; }
    void setCell(std::size_t i, int value) { mCells[i] = value; }
    [[nodiscard]] int cellCount() const { return static_cast<int>(mCells.size()); }
    void add(int value) { mCells.push_back(value); }

private:
    std::vector<int> mCells;
};

struct Labels {
    std::vector<std::string> mNames{"a", "b"};
    [[nodiscard]] std::size_t size() const { return mNames.size(); }
};

struct Faulty {
    [[nodiscard]] std::size_t size() const { return 2; }
};
inline int faultyAt(const Faulty&, std::size_t i) {
    if (i == 1) {
        throw std::runtime_error("cell 1 is broken");
    }
    return 7;
}

struct Point {
    Point() = default;
    explicit Point(int x) : mX(x) {}
    int mX = 0;
};

struct Path {
    std::vector<Point> mPoints;
    void add(int x) { mPoints.emplace_back(x); }
    [[nodiscard]] std::size_t size() const { return mPoints.size(); }
    Point& operator[](std::size_t i) { return mPoints[i]; }
};

struct LivePath : Path {};

struct Tags {
    std::set<std::string> mTags;
    void add(const std::string& t) { mTags.insert(t); }
    [[nodiscard]] auto begin() const { return mTags.begin(); }
    [[nodiscard]] auto end() const { return mTags.end(); }
};

struct Vec {
    Vec() = default;
    Vec(double x, double y) : mX(x), mY(y) {}
    double mX = 0;
    double mY = 0;
    friend bool operator==(const Vec&, const Vec&) = default;
};

struct Resource {
    bool mClosed = false;
    void close() { mClosed = true; }
    [[nodiscard]] std::string describe() const {
        return mClosed ? "Resource(closed)" : "Resource(open)";
    }
};

struct Tagged {};
inline std::string taggedKind() {
    return "TaggedClass";
}

struct Mod;

} // namespace IdiomTest

template <> struct std::formatter<IdiomTest::Vec> : std::formatter<std::string> {
    auto format(const IdiomTest::Vec& v, std::format_context& ctx) const {
        return std::formatter<std::string>::format(std::format("({}, {})", v.mX, v.mY), ctx);
    }
};

template <> struct Alka::Bind<IdiomTest::Numbers> : Class<IdiomTest::Numbers> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&IdiomTest::Numbers::add>(),
        Alka::prop<"length", &IdiomTest::Numbers::size>(Alka::int64As<Alka::Int64Mode::Number>),
        Alka::iterable<IdiomTest::Numbers>(), indexer());
};

template <> struct Alka::Bind<IdiomTest::MoreNumbers> : Class<IdiomTest::MoreNumbers> {
    using Extends = IdiomTest::Numbers;
    static constexpr auto members = Alka::members(Alka::ctor<>());
};

template <> struct Alka::Bind<IdiomTest::Point> : Class<IdiomTest::Point> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(), Alka::prop<&IdiomTest::Point::mX>());
};

template <> struct Alka::Bind<IdiomTest::Path> : Class<IdiomTest::Path> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&IdiomTest::Path::add>(),
                      Alka::iterable<IdiomTest::Path>(), indexer());
};

template <> struct Alka::Bind<IdiomTest::LivePath> : Class<IdiomTest::LivePath> {
    using Extends = IdiomTest::Path;
    static constexpr auto members = Alka::members(Alka::ctor<>(), indexer(policy::internalRef));
};

template <> struct Alka::Bind<IdiomTest::Tags> : Class<IdiomTest::Tags> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&IdiomTest::Tags::add>(), Alka::iterable<IdiomTest::Tags>());
};

template <> struct Alka::Bind<IdiomTest::Vec> : Class<IdiomTest::Vec> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<double, double>(), Alka::prop<&IdiomTest::Vec::mX>(),
                      Alka::prop<&IdiomTest::Vec::mY>(), Alka::toStringFromFormat<IdiomTest::Vec>(),
                      Alka::toJSON<IdiomTest::Vec>(), Alka::equals<IdiomTest::Vec>());
};

template <> struct Alka::Bind<IdiomTest::Grid> : Class<IdiomTest::Grid> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&IdiomTest::Grid::add>(),
                      Alka::indexer<&IdiomTest::Grid::cell, &IdiomTest::Grid::setCell,
                                    &IdiomTest::Grid::cellCount>());
};

template <> struct Alka::Bind<IdiomTest::Labels> : Class<IdiomTest::Labels> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(),
        Alka::indexer<[](const IdiomTest::Labels& l, std::size_t i) { return l.mNames[i]; }>());
};

template <> struct Alka::Bind<IdiomTest::Faulty> : Class<IdiomTest::Faulty> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::indexer<&IdiomTest::faultyAt>());
};

template <> struct Alka::Bind<IdiomTest::Resource> : Class<IdiomTest::Resource> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::prop<"closed", &IdiomTest::Resource::mClosed>(readonly),
        Alka::method<"@@dispose", &IdiomTest::Resource::close>(),
        Alka::method<"@@nodejs.util.inspect.custom", &IdiomTest::Resource::describe>());
};

template <> struct Alka::Bind<IdiomTest::Tagged> : Class<IdiomTest::Tagged> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(),
                      Alka::prop<"@@toStringTag",
                                 [](const IdiomTest::Tagged&) { return std::string("Tagged!"); }>(),
                      Alka::staticProp<"@@toStringTag", &IdiomTest::taggedKind>());
};

template <> struct Alka::Bind<IdiomTest::Mod> : Module<"idioms"> {
    static constexpr auto members = Alka::members(
        Alka::type<IdiomTest::Numbers>, Alka::type<IdiomTest::MoreNumbers>,
        Alka::type<IdiomTest::Point>, Alka::type<IdiomTest::Path>, Alka::type<IdiomTest::LivePath>,
        Alka::type<IdiomTest::Tags>, Alka::type<IdiomTest::Vec>, Alka::type<IdiomTest::Grid>,
        Alka::type<IdiomTest::Labels>, Alka::type<IdiomTest::Faulty>,
        Alka::type<IdiomTest::Resource>, Alka::type<IdiomTest::Tagged>);
};

namespace {

struct IdiomFixture : AlkaTest::V8Fixture {
    IdiomFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<IdiomTest::Mod>(context);
        });
        evalString("globalThis.n = new idioms.Numbers(); n.add(1); n.add(2); n.add(3); 'ok'");
    }
};

} // namespace

TEST(Idioms, ContainersIterateLikeJSIterables) {
    IdiomFixture fx;
    EXPECT_EQ(fx.evalString("[...n].join(',')"), "1,2,3");
    EXPECT_EQ(fx.evalInt("(() => { let s = 0; for (const v of n) s += v; return s; })()"), 6);
    EXPECT_EQ(fx.evalString("Array.from(n, v => v * 10).join(',')"), "10,20,30");
    EXPECT_EQ(fx.evalString("(() => { const it = n[Symbol.iterator](); it.next(); n.add(4);"
                            " return [...it].join(','); })()"),
              "2,3,4");
    EXPECT_EQ(fx.evalString("(() => { const t = new idioms.Tags(); t.add('b'); t.add('a');"
                            " return [...t].join(','); })()"),
              "a,b");
}

TEST(Idioms, IndexersReadWriteAndEnumerate) {
    IdiomFixture fx;
    EXPECT_EQ(fx.evalInt("n[1]"), 2);
    fx.evalString("n[1] = 20; 'ok'");
    EXPECT_EQ(fx.evalInt("n[1]"), 20);
    EXPECT_TRUE(fx.evalBool("n[99] === undefined && !(99 in n) && (2 in n)"));
    fx.evalString("n[99] = 5; 'ok'"); // ignored, like a typed array
    EXPECT_EQ(fx.evalInt("n.length"), 3);
    EXPECT_EQ(fx.evalString("Object.keys(n).join(',')"), "0,1,2");
    EXPECT_EQ(fx.evalString("try { n[0] = 'x' } catch (e) { e.message }"),
              "Numbers.[index]: expected number, got string");
    EXPECT_EQ(
        fx.evalInt("(() => { const m = new idioms.MoreNumbers(); m.add(7); return m[0]; })()"), 7);
}

TEST(Idioms, IndexersThroughCustomAccessors) {
    IdiomFixture fx;
    fx.evalString("globalThis.g = new idioms.Grid(); g.add(1); g.add(2); 'ok'");
    EXPECT_EQ(fx.evalInt("g[1]"), 2);
    fx.evalString("g[1] = 20; 'ok'");
    EXPECT_EQ(fx.evalInt("g[1]"), 20);
    EXPECT_TRUE(fx.evalBool("g[5] === undefined && !(5 in g) && (1 in g)"));
    fx.evalString("g[5] = 9; 'ok'"); // out of range: ignored
    EXPECT_EQ(fx.evalString("Object.keys(g).join(',')"), "0,1");
    EXPECT_EQ(fx.evalString("try { g[0] = 'x' } catch (e) { e.message }"),
              "Grid.[index]: expected number, got string");
    fx.evalString("globalThis.l = new idioms.Labels(); l[0] = 'z'; 'ok'");
    EXPECT_EQ(fx.evalString("l[0] + l[1]"), "ab");
    EXPECT_EQ(fx.evalString("Object.keys(l).join(',')"), "0,1");
    EXPECT_EQ(fx.evalInt("new idioms.Faulty()[0]"), 7);
    EXPECT_EQ(fx.evalString("try { new idioms.Faulty()[1] } catch (e) { e.message }"),
              "cell 1 is broken");
}

TEST(Idioms, BoundClassElementsAreCopiesUnlessDeclaredLive) {
    IdiomFixture fx;
    EXPECT_EQ(fx.evalInt("(() => { const p = new idioms.Path(); p.add(1);"
                         " const first = p[0]; first.x = 9; return p[0].x; })()"),
              1);
    EXPECT_EQ(fx.evalInt("(() => { const p = new idioms.LivePath(); p.add(1);"
                         " const first = p[0]; first.x = 9; return p[0].x; })()"),
              9);
    EXPECT_EQ(fx.evalString("(() => { const p = new idioms.Path(); p.add(4); p.add(5);"
                            " return [...p].map(q => q.x).join(','); })()"),
              "4,5");
}

TEST(Idioms, ToStringToJSONAndEquals) {
    IdiomFixture fx;
    EXPECT_EQ(fx.evalString("String(new idioms.Vec(1, 2.5))"), "(1, 2.5)");
    EXPECT_EQ(fx.evalString("`${new idioms.Vec(3, 4)}`"), "(3, 4)");
    EXPECT_EQ(fx.evalString("JSON.stringify({ v: new idioms.Vec(1, 2) })"),
              R"({"v":{"x":1,"y":2}})");
    EXPECT_TRUE(fx.evalBool("new idioms.Vec(1, 2).equals(new idioms.Vec(1, 2))"));
    EXPECT_FALSE(fx.evalBool("new idioms.Vec(1, 2).equals(new idioms.Vec(2, 1))"));
}

TEST(Idioms, SymbolNamesBindWellKnownSymbolsAnyOtherNameARegistrySymbol) {
    IdiomFixture fx;
    fx.evalString("globalThis.r = new idioms.Resource(); 'ok'");
    EXPECT_EQ(fx.evalString("r[Symbol.for('nodejs.util.inspect.custom')]()"), "Resource(open)");
    EXPECT_EQ(fx.evalString("idioms.Resource.prototype[Symbol.for('nodejs.util.inspect.custom')]"
                            ".name"),
              "[nodejs.util.inspect.custom]");
    EXPECT_TRUE(fx.evalBool("r[Symbol.for('dispose')] === undefined"));
    if (fx.evalBool("typeof Symbol.dispose === 'symbol'")) {
        EXPECT_EQ(fx.evalString("idioms.Resource.prototype[Symbol.dispose].name"),
                  "[Symbol.dispose]");
        EXPECT_TRUE(
            fx.evalBool("(() => { let kept; { using res = new idioms.Resource(); kept = res; }"
                        " return kept.closed; })()"));
    } else {
        EXPECT_TRUE(
            fx.evalBool("Object.getOwnPropertySymbols(idioms.Resource.prototype).length === 1"));
    }
}

TEST(Idioms, SymbolNamesKeyPropsAndStaticPropsTooInJSAndInTheDts) {
    IdiomFixture fx;
    EXPECT_EQ(fx.evalString("Object.prototype.toString.call(new idioms.Tagged())"),
              "[object Tagged!]");
    EXPECT_EQ(fx.evalString("Object.prototype.toString.call(idioms.Tagged)"),
              "[object TaggedClass]");
    const auto dts = Alka::generateDts<IdiomTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("    readonly [Symbol.toStringTag]: string;"));
    EXPECT_THAT(dts, testing::HasSubstr("static readonly [Symbol.toStringTag]: string;"));
    EXPECT_THAT(dts, testing::Not(testing::HasSubstr("@@")));
}

TEST(Idioms, InTheDts) {
    const auto dts = Alka::generateDts<IdiomTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("[Symbol.iterator](): IterableIterator<number>;"));
    EXPECT_THAT(dts, testing::HasSubstr("[index: number]: number;"));
    EXPECT_THAT(dts, testing::HasSubstr("[Symbol.iterator](): IterableIterator<string>;"));
    EXPECT_THAT(dts, testing::HasSubstr("toString(): string;"));
    EXPECT_THAT(dts, testing::HasSubstr("equals(other: idioms.Vec): boolean;"));
    EXPECT_THAT(dts, testing::HasSubstr("toJSON(): object;"));
    const auto grid = dts.substr(dts.find("class Grid {"));
    EXPECT_THAT(grid.substr(0, grid.find('}')), testing::HasSubstr("    [index: number]: number;"));
    const auto labels = dts.substr(dts.find("class Labels {"));
    EXPECT_THAT(labels.substr(0, labels.find('}')),
                testing::HasSubstr("readonly [index: number]: string;"));
    EXPECT_THAT(dts, testing::HasSubstr("[Symbol.dispose](): void;"));
    EXPECT_THAT(dts,
                testing::HasSubstr(R"(// [Symbol.for("nodejs.util.inspect.custom")](): string;)"));
}
