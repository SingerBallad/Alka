#include <Alka/FixedString.hpp>
#include <Alka/NameOf.hpp>
#include <Alka/NamePolicy.hpp>

#include <doctest/doctest.h>

#include <functional>
#include <string_view>
#include <vector>

namespace AlkaNameTest {

struct Entity {
    int mHp = 0;
    void update(double) {}
    static int count() { return 0; }
};

namespace Deep {
struct Bot {};
}  // namespace Deep

class Player : public Entity {
public:
    void attack(Entity&) {}
    int mMaxSpeed = 0;
};

struct Overloaded {
    void f(int) {}
    void f(double) {}
};

// Multi-base classes get "fat" member pointers, rendered as aggregates by MSVC.
struct BaseA {
    virtual ~BaseA() = default;
    int mLeft = 0;
};
struct BaseB {
    int mRight = 0;
};
struct MultiBase : BaseA, BaseB {
    int poke() { return 1; }
    int mOwn = 0;
};

inline int spawnThing(int v) { return v; }
inline std::function<int(int)> fnFactory() { return {}; }
inline void takeCallback(const std::function<void(int)>&) {}

enum class Color { Red, Green = 5, Blue };
enum Legacy { LegacyA = -3, LegacyB = 1 };

struct Outer {
    struct Inner {};
};

}  // namespace AlkaNameTest

using namespace std::string_view_literals;

// ---- type names -----------------------------------------------------------
static_assert(Alka::typeNameOf<AlkaNameTest::Player>.view() == "Player"sv);
static_assert(Alka::typeNameOf<AlkaNameTest::Deep::Bot>.view() == "Bot"sv);
static_assert(Alka::typeNameOf<AlkaNameTest::Outer::Inner>.view() == "Inner"sv);
static_assert(Alka::qualifiedTypeNameOf<AlkaNameTest::Player>.view() == "AlkaNameTest::Player"sv);
static_assert(Alka::hasIdentifierTypeName<AlkaNameTest::Player>());
static_assert(!Alka::hasIdentifierTypeName<std::vector<int>>());

// ---- value names ----------------------------------------------------------
static_assert(Alka::valueNameOf<&AlkaNameTest::Player::attack>.view() == "attack"sv);
static_assert(Alka::valueNameOf<&AlkaNameTest::Entity::mHp>.view() == "mHp"sv);
static_assert(Alka::valueNameOf<&AlkaNameTest::Entity::update>.view() == "update"sv);
static_assert(Alka::valueNameOf<&AlkaNameTest::Entity::count>.view() == "count"sv);
static_assert(Alka::valueNameOf<&AlkaNameTest::spawnThing>.view() == "spawnThing"sv);
// parens inside return/parameter types must not confuse the parser
static_assert(Alka::valueNameOf<&AlkaNameTest::fnFactory>.view() == "fnFactory"sv);
static_assert(Alka::valueNameOf<&AlkaNameTest::takeCallback>.view() == "takeCallback"sv);
static_assert(Alka::valueNameOf<static_cast<void (AlkaNameTest::Overloaded::*)(int)>(
                  &AlkaNameTest::Overloaded::f)>.view() == "f"sv);
static_assert(Alka::valueNameOf<&AlkaNameTest::MultiBase::poke>.view() == "poke"sv);
static_assert(Alka::valueNameOf<&AlkaNameTest::MultiBase::mOwn>.view() == "mOwn"sv);
static_assert(Alka::valueNameOf<&AlkaNameTest::MultiBase::mRight>.view() == "mRight"sv);

// ---- enum reflection ------------------------------------------------------
static_assert(Alka::valueNameOf<AlkaNameTest::Color::Red>.view() == "Red"sv);
static_assert(Alka::valueNameOf<AlkaNameTest::LegacyB>.view() == "LegacyB"sv);

static_assert(Alka::enumEntriesOf<AlkaNameTest::Color>.size() == 3);
static_assert(Alka::enumEntriesOf<AlkaNameTest::Color>[0].mName == "Red"sv);
static_assert(Alka::enumEntriesOf<AlkaNameTest::Color>[1].mValue == AlkaNameTest::Color::Green);
static_assert(Alka::enumEntriesOf<AlkaNameTest::Color>[1].mName == "Green"sv);
static_assert(Alka::enumEntriesOf<AlkaNameTest::Color>[2].mName == "Blue"sv);

static_assert(Alka::enumEntriesOf<AlkaNameTest::Legacy>.size() == 2);
static_assert(Alka::enumEntriesOf<AlkaNameTest::Legacy>[0].mValue == AlkaNameTest::LegacyA);
static_assert(Alka::enumEntriesOf<AlkaNameTest::Legacy>[0].mName == "LegacyA"sv);

static_assert(Alka::enumNameOf(AlkaNameTest::Color::Blue) == "Blue"sv);
static_assert(Alka::enumNameOf(static_cast<AlkaNameTest::Color>(42)).empty());

// ---- naming policy --------------------------------------------------------
static_assert(Alka::Detail::transformedName<Alka::NameKind::Property,
                                            Alka::FixedString{"mHp"}>.view() == "hp"sv);
static_assert(Alka::Detail::transformedName<Alka::NameKind::Property,
                                            Alka::FixedString{"mMaxSpeed"}>.view() == "maxSpeed"sv);
static_assert(Alka::Detail::transformedName<Alka::NameKind::Property,
                                            Alka::FixedString{"hp"}>.view() == "hp"sv);
static_assert(Alka::Detail::transformedName<Alka::NameKind::Property,
                                            Alka::FixedString{"m"}>.view() == "m"sv);
static_assert(Alka::Detail::transformedName<Alka::NameKind::Property,
                                            Alka::FixedString{"mx"}>.view() == "mx"sv);
static_assert(Alka::Detail::transformedName<Alka::NameKind::Method,
                                            Alka::FixedString{"mHp"}>.view() == "mHp"sv);

// ---- FixedString ----------------------------------------------------------
static_assert(Alka::FixedString{"abc"}.size() == 3);
static_assert(Alka::FixedString{"foo"} + Alka::FixedString{"bar"} == Alka::FixedString{"foobar"});
static_assert(Alka::FixedString{"abc"} == "abc"sv);
static_assert(Alka::FixedString<3>::from("xyz123"sv) == "xyz"sv);

TEST_CASE("enum name lookup at runtime") {
    CHECK(Alka::enumNameOf(AlkaNameTest::Color::Green) == "Green"sv);
    CHECK(Alka::enumNameOf(static_cast<AlkaNameTest::Color>(7)).empty());
    CHECK(Alka::valueNameOf<&AlkaNameTest::Player::attack>.view() == "attack"sv);
}
