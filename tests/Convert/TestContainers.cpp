#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <list>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace ContainerTest {

struct Item {
    Item() = default;
    explicit Item(int v) : mWorth(v) {}
    int mWorth = 0;
};

inline int sum(const std::vector<int>& values) {
    return std::accumulate(values.begin(), values.end(), 0);
}
inline std::vector<std::string> tags() {
    return {"a", "b", "c"};
}
inline std::vector<std::vector<int>> grid() {
    return {{1, 2}, {3}};
}
inline int keyTotal(const std::map<std::string, int>& m) {
    int total = 0;
    for (const auto& [k, v] : m) {
        total += static_cast<int>(k.size()) * v;
    }
    return total;
}
inline std::map<std::string, int> stats() {
    return {{"hp", 10}, {"mp", 4}};
}
inline std::map<int, std::string> lookup() {
    return {{1, "one"}, {2, "two"}};
}
inline int setSize(const std::set<int>& s) {
    return static_cast<int>(s.size());
}
inline std::set<int> primes() {
    return {2, 3, 5};
}
inline int orZero(std::optional<int> v) {
    return v.value_or(0);
}
inline std::optional<int> half(int v) {
    return v % 2 == 0 ? std::optional(v / 2) : std::nullopt;
}
inline std::tuple<int, std::string> pairUp(int v) {
    return {v, std::to_string(v)};
}
inline int tupleFirst(const std::tuple<int, std::string>& t) {
    return std::get<0>(t);
}
inline std::string pickVariant(const std::variant<int, bool>& v) {
    return std::holds_alternative<int>(v) ? "int" : "bool";
}
inline std::vector<std::byte> echoBytes(std::vector<std::byte> b) {
    for (auto& x : b) {
        x = static_cast<std::byte>(static_cast<unsigned char>(x) + 1);
    }
    return b;
}
inline std::vector<Item> makeItems() {
    return {Item(1), Item(2)};
}
inline std::deque<int> reversed(std::deque<int> values) {
    std::ranges::reverse(values);
    return values;
}
inline std::list<std::string> listed(const std::list<std::string>& values) {
    auto out = values;
    out.push_front("first");
    return out;
}
inline std::chrono::milliseconds doubled(std::chrono::milliseconds d) {
    return d * 2;
}
inline double inSeconds(std::chrono::duration<double> d) {
    return d.count();
}
inline std::chrono::seconds wholeSeconds(std::chrono::seconds d) {
    return d;
}
inline std::chrono::system_clock::time_point dayAfter(std::chrono::system_clock::time_point t) {
    return t + std::chrono::hours(24);
}
inline int itemTotal(const std::vector<Item>& items) {
    int total = 0;
    for (const auto& item : items) {
        total += item.mWorth;
    }
    return total;
}

struct BoxMod;

} // namespace ContainerTest

template <> struct Alka::Bind<ContainerTest::Item> : Class<ContainerTest::Item> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(arg("worth")), Alka::prop<&ContainerTest::Item::mWorth>());
};

template <> struct Alka::Bind<ContainerTest::BoxMod> : Module<"box"> {
    static constexpr auto members = Alka::members(
        Alka::type<ContainerTest::Item>, Alka::fn<&ContainerTest::sum>(arg("values")),
        Alka::fn<&ContainerTest::tags>(), Alka::fn<&ContainerTest::grid>(),
        Alka::fn<&ContainerTest::keyTotal>(), Alka::fn<&ContainerTest::stats>(),
        Alka::fn<&ContainerTest::lookup>(), Alka::fn<&ContainerTest::setSize>(),
        Alka::fn<&ContainerTest::primes>(), Alka::fn<&ContainerTest::orZero>(),
        Alka::fn<&ContainerTest::half>(), Alka::fn<&ContainerTest::pairUp>(),
        Alka::fn<&ContainerTest::tupleFirst>(), Alka::fn<&ContainerTest::pickVariant>(),
        Alka::fn<&ContainerTest::echoBytes>(), Alka::fn<&ContainerTest::makeItems>(),
        Alka::fn<&ContainerTest::itemTotal>(), Alka::fn<&ContainerTest::reversed>(),
        Alka::fn<&ContainerTest::listed>(), Alka::fn<&ContainerTest::doubled>(),
        Alka::fn<&ContainerTest::inSeconds>(), Alka::fn<&ContainerTest::wholeSeconds>(),
        Alka::fn<&ContainerTest::dayAfter>());
};

namespace {

struct BoxFixture : AlkaTest::V8Fixture {
    BoxFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<ContainerTest::BoxMod>(context);
        });
    }
};

} // namespace

TEST(Containers, VectorsRoundTripAsArrays) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalInt("box.sum([1, 2, 3, 4])"), 10);
    EXPECT_EQ(fx.evalString("box.tags().join('-')"), "a-b-c");
    EXPECT_TRUE(fx.evalBool("Array.isArray(box.tags())"));
    EXPECT_EQ(fx.evalInt("box.grid()[0][1]"), 2);
    EXPECT_EQ(fx.evalString("try { box.sum([1, 'x']) } catch (e) { e.message }"),
              "arg 1 (values) of box.sum: element 1: expected number, got string");
    EXPECT_EQ(fx.evalString("try { box.sum(5) } catch (e) { e.message }"),
              "arg 1 (values) of box.sum: expected array or Int32Array, got number");
    // holes read as undefined, at their index (not skipped)
    EXPECT_EQ(fx.evalString("try { box.sum([1, , 3]) } catch (e) { e.message }"),
              "arg 1 (values) of box.sum: element 1: expected number, got undefined");
}

TEST(Containers, StringKeyedMapsRoundTripAsPlainObjects) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalInt("box.keyTotal({ ab: 2, c: 3 })"), 7);
    EXPECT_EQ(fx.evalInt("box.keyTotal({ ab: 2, [Symbol('s')]: 3 })"), 4);
    EXPECT_EQ(fx.evalInt("box.stats().hp"), 10);
    EXPECT_EQ(fx.evalString("Object.keys(box.stats()).sort().join(',')"), "hp,mp");
}

TEST(Containers, DequesAndListsRoundTripAsArrays) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalString("box.reversed([1, 2, 3]).join(',')"), "3,2,1");
    EXPECT_EQ(fx.evalString("box.reversed(new Int32Array([4, 5])).join(',')"), "5,4");
    EXPECT_EQ(fx.evalString("box.listed(['a', 'b']).join(',')"), "first,a,b");
}

TEST(Containers, DurationsAreMillisecondsTimePointsAreDates) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalInt("box.doubled(250)"), 500);
    EXPECT_EQ(fx.evalDouble("box.inSeconds(1500)"), 1.5);
    EXPECT_EQ(fx.evalInt("box.wholeSeconds(3000)"), 3000);
    EXPECT_THAT(fx.evalString("try { box.wholeSeconds(1500) } catch (e) { e.message }"),
                testing::StartsWith(
                    "arg 1 of box.wholeSeconds: this duration's unit cannot represent 1500"));
    EXPECT_TRUE(fx.evalBool("box.dayAfter(new Date(0)) instanceof Date"));
    EXPECT_EQ(fx.evalString("box.dayAfter(new Date(Date.UTC(2026, 0, 1))).toISOString()"),
              "2026-01-02T00:00:00.000Z");
    EXPECT_EQ(fx.evalString("box.dayAfter(0).toISOString()"), "1970-01-02T00:00:00.000Z");
    EXPECT_EQ(fx.evalString("try { box.dayAfter(new Date(NaN)) } catch (e) { e.message }"),
              "arg 1 of box.dayAfter: an invalid Date");
    const auto dts = Alka::generateDts<ContainerTest::BoxMod>();
    EXPECT_THAT(dts, testing::HasSubstr("function doubled(arg0: number): number;"));
    EXPECT_THAT(dts, testing::HasSubstr("function dayAfter(arg0: Date | number): Date;"));
}

TEST(Containers, NonStringKeyedMapsRoundTripAsMap) {
    BoxFixture fx;
    EXPECT_TRUE(fx.evalBool("box.lookup() instanceof Map"));
    EXPECT_EQ(fx.evalString("box.lookup().get(2)"), "two");
}

TEST(Containers, SetsRoundTripAsSet) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalInt("box.setSize(new Set([1, 2, 2, 3]))"), 3);
    EXPECT_TRUE(fx.evalBool("box.primes() instanceof Set"));
    EXPECT_TRUE(fx.evalBool("box.primes().has(5)"));
}

TEST(Containers, OptionalMapsToUndefinedOrT) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalInt("box.orZero(7)"), 7);
    EXPECT_EQ(fx.evalInt("box.orZero(null)"), 0);
    EXPECT_EQ(fx.evalInt("box.orZero(undefined)"), 0);
    EXPECT_EQ(fx.evalInt("box.orZero()"), 0);
    EXPECT_EQ(fx.evalInt("box.half(8)"), 4);
    EXPECT_TRUE(fx.evalBool("box.half(7) === undefined"));
}

TEST(Containers, TuplesRoundTripAsFixedLengthArrays) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalString("JSON.stringify(box.pairUp(3))"), R"([3,"3"])");
    EXPECT_EQ(fx.evalInt("box.tupleFirst([5, 'five'])"), 5);
    EXPECT_EQ(fx.evalString("try { box.tupleFirst([5]) } catch (e) { e.message }"),
              "arg 1 of box.tupleFirst: expected a tuple of length 2, got length 1");
}

TEST(Containers, VariantsTakeTheFirstMatchingAlternative) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalString("box.pickVariant(3)"), "int");
    EXPECT_EQ(fx.evalString("box.pickVariant(true)"), "bool");
    EXPECT_EQ(fx.evalString("try { box.pickVariant('nope') } catch (e) { e.message }"),
              "arg 1 of box.pickVariant: expected int | bool, got string");
}

TEST(Containers, ByteVectorsRoundTripAsUint8Array) {
    BoxFixture fx;
    EXPECT_TRUE(fx.evalBool("box.echoBytes(new Uint8Array([1, 2, 3])) instanceof Uint8Array"));
    EXPECT_EQ(fx.evalString("Array.from(box.echoBytes(new Uint8Array([1, 2, 3]))).join(',')"),
              "2,3,4");
    EXPECT_EQ(fx.evalString("Array.from(box.echoBytes(new Uint8Array([255]))).join(',')"), "0");
}

TEST(Containers, BoundClassesRideContainersByValue) {
    BoxFixture fx;
    EXPECT_EQ(fx.evalInt("box.makeItems().length"), 2);
    EXPECT_TRUE(fx.evalBool("box.makeItems()[0] instanceof box.Item"));
    EXPECT_EQ(fx.evalInt("box.itemTotal([new box.Item(3), new box.Item(4)])"), 7);
    EXPECT_EQ(
        fx.evalInt("(() => { const i = new box.Item(5); box.itemTotal([i]); return i.worth; })()"),
        5);
}
