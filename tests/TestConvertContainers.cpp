#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <doctest/doctest.h>

#include <cstddef>
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
inline std::vector<std::string> tags() { return {"a", "b", "c"}; }
inline std::vector<std::vector<int>> grid() { return {{1, 2}, {3}}; }
inline int keyTotal(const std::map<std::string, int>& m) {
    int total = 0;
    for (const auto& [k, v] : m) {
        total += static_cast<int>(k.size()) * v;
    }
    return total;
}
inline std::map<std::string, int> stats() { return {{"hp", 10}, {"mp", 4}}; }
inline std::map<int, std::string> lookup() { return {{1, "one"}, {2, "two"}}; }
inline int setSize(const std::set<int>& s) { return static_cast<int>(s.size()); }
inline std::set<int> primes() { return {2, 3, 5}; }
inline int orZero(std::optional<int> v) { return v.value_or(0); }
inline std::optional<int> half(int v) {
    return v % 2 == 0 ? std::optional<int>(v / 2) : std::nullopt;
}
inline std::tuple<int, std::string> pairUp(int v) { return {v, std::to_string(v)}; }
inline int tupleFirst(const std::tuple<int, std::string>& t) { return std::get<0>(t); }
inline std::string pickVariant(const std::variant<int, bool>& v) {
    return std::holds_alternative<int>(v) ? "int" : "bool";
}
inline std::vector<std::byte> echoBytes(std::vector<std::byte> b) {
    for (auto& x : b) {
        x = static_cast<std::byte>(static_cast<unsigned char>(x) + 1);
    }
    return b;
}
inline std::vector<Item> makeItems() { return {Item(1), Item(2)}; }
inline int itemTotal(const std::vector<Item>& items) {
    int total = 0;
    for (const auto& item : items) {
        total += item.mWorth;
    }
    return total;
}

struct BoxMod;

}  // namespace ContainerTest

template<>
struct Alka::Bind<ContainerTest::Item> : Alka::Class<ContainerTest::Item> {
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(Alka::arg("worth")),
                      Alka::prop<&ContainerTest::Item::mWorth>());
};

template<>
struct Alka::Bind<ContainerTest::BoxMod> : Alka::Module<"box"> {
    static constexpr auto members = Alka::members(
        Alka::type<ContainerTest::Item>,
        Alka::fn<&ContainerTest::sum>(Alka::arg("values")),
        Alka::fn<&ContainerTest::tags>(), Alka::fn<&ContainerTest::grid>(),
        Alka::fn<&ContainerTest::keyTotal>(), Alka::fn<&ContainerTest::stats>(),
        Alka::fn<&ContainerTest::lookup>(), Alka::fn<&ContainerTest::setSize>(),
        Alka::fn<&ContainerTest::primes>(), Alka::fn<&ContainerTest::orZero>(),
        Alka::fn<&ContainerTest::half>(), Alka::fn<&ContainerTest::pairUp>(),
        Alka::fn<&ContainerTest::tupleFirst>(), Alka::fn<&ContainerTest::pickVariant>(),
        Alka::fn<&ContainerTest::echoBytes>(), Alka::fn<&ContainerTest::makeItems>(),
        Alka::fn<&ContainerTest::itemTotal>());
};

namespace {

struct BoxFixture : AlkaTest::V8Fixture {
    BoxFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<ContainerTest::BoxMod>(context);
        });
    }
};

}  // namespace

TEST_CASE("vectors <-> arrays") {
    BoxFixture fx;
    CHECK(fx.evalInt("box.sum([1, 2, 3, 4])") == 10);
    CHECK(fx.evalString("box.tags().join('-')") == "a-b-c");
    CHECK(fx.evalBool("Array.isArray(box.tags())"));
    CHECK(fx.evalInt("box.grid()[0][1]") == 2);
    CHECK(fx.evalString("try { box.sum([1, 'x']) } catch (e) { e.message }") ==
          "arg 1 (values) of box.sum: element 1: expected number, got string");
    CHECK(fx.evalString("try { box.sum(5) } catch (e) { e.message }") ==
          "arg 1 (values) of box.sum: expected array, got number");
}

TEST_CASE("string-keyed maps <-> plain objects") {
    BoxFixture fx;
    CHECK(fx.evalInt("box.keyTotal({ ab: 2, c: 3 })") == 7);
    CHECK(fx.evalInt("box.stats().hp") == 10);
    CHECK(fx.evalString("Object.keys(box.stats()).sort().join(',')") == "hp,mp");
}

TEST_CASE("non-string-keyed maps <-> Map") {
    BoxFixture fx;
    CHECK(fx.evalBool("box.lookup() instanceof Map"));
    CHECK(fx.evalString("box.lookup().get(2)") == "two");
}

TEST_CASE("sets <-> Set") {
    BoxFixture fx;
    CHECK(fx.evalInt("box.setSize(new Set([1, 2, 2, 3]))") == 3);
    CHECK(fx.evalBool("box.primes() instanceof Set"));
    CHECK(fx.evalBool("box.primes().has(5)"));
}

TEST_CASE("optional maps to undefined | T") {
    BoxFixture fx;
    CHECK(fx.evalInt("box.orZero(7)") == 7);
    CHECK(fx.evalInt("box.orZero(null)") == 0);
    CHECK(fx.evalInt("box.orZero(undefined)") == 0);
    CHECK(fx.evalInt("box.orZero()") == 0);
    CHECK(fx.evalInt("box.half(8)") == 4);
    CHECK(fx.evalBool("box.half(7) === undefined"));
}

TEST_CASE("tuples <-> fixed-length arrays") {
    BoxFixture fx;
    CHECK(fx.evalString("JSON.stringify(box.pairUp(3))") == R"([3,"3"])");
    CHECK(fx.evalInt("box.tupleFirst([5, 'five'])") == 5);
    CHECK(fx.evalString("try { box.tupleFirst([5]) } catch (e) { e.message }") ==
          "arg 1 of box.tupleFirst: expected a tuple of length 2, got length 1");
}

TEST_CASE("variants take the first matching alternative") {
    BoxFixture fx;
    CHECK(fx.evalString("box.pickVariant(3)") == "int");
    CHECK(fx.evalString("box.pickVariant(true)") == "bool");
    CHECK(fx.evalString("try { box.pickVariant('nope') } catch (e) { e.message }") ==
          "arg 1 of box.pickVariant: expected int | bool, got string");
}

TEST_CASE("byte vectors <-> Uint8Array") {
    BoxFixture fx;
    CHECK(fx.evalBool("box.echoBytes(new Uint8Array([1, 2, 3])) instanceof Uint8Array"));
    CHECK(fx.evalString("Array.from(box.echoBytes(new Uint8Array([1, 2, 3]))).join(',')") ==
          "2,3,4");
    CHECK(fx.evalString("Array.from(box.echoBytes(new Uint8Array([255]))).join(',')") == "0");
}

TEST_CASE("bound classes ride containers by value") {
    BoxFixture fx;
    CHECK(fx.evalInt("box.makeItems().length") == 2);
    CHECK(fx.evalBool("box.makeItems()[0] instanceof box.Item"));
    CHECK(fx.evalInt("box.itemTotal([new box.Item(3), new box.Item(4)])") == 7);
    // copies are independent of the originals
    CHECK(fx.evalInt("(() => { const i = new box.Item(5); box.itemTotal([i]); return i.worth; })()") ==
          5);
}
