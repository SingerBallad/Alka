#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace NamingTest {

using Alka::NameKind;

template <class N>
constexpr bool names(const N& naming, const NameKind kind, const std::string_view cxx,
                     const std::string_view js) {
    return naming.apply(kind, cxx) == js;
}

constexpr auto P = NameKind::Property;
constexpr auto M = NameKind::Method;

static_assert(names(Alka::verbatim, P, "mHp", "mHp"));

static_assert(names(Alka::memberPrefixes, P, "mHp", "hp"));
static_assert(names(Alka::memberPrefixes, P, "m_max_speed", "max_speed"));
static_assert(names(Alka::memberPrefixes, P, "mURLPath", "urlPath"));
static_assert(names(Alka::memberPrefixes, P, "mIDs", "ids"));
static_assert(names(Alka::memberPrefixes, P, "make", "make"));
static_assert(names(Alka::memberPrefixes, M, "mHp", "mHp"));
static_assert(names(Alka::memberPrefixes, NameKind::StaticProperty, "sCount", "count"));

constexpr auto camel = Alka::naming(Alka::camelCase);
static_assert(names(camel, P, "max_speed", "maxSpeed"));
static_assert(names(camel, P, "MAX_SPEED", "maxSpeed"));
static_assert(names(camel, M, "GetHealth", "getHealth"));
static_assert(names(camel, M, "HTTPServer_port", "httpServerPort"));
static_assert(names(camel, M, "parseURLPath", "parseURLPath"));
static_assert(names(camel, P, "_hidden", "_hidden"));
static_assert(names(camel, NameKind::Type, "Player", "Player"));
static_assert(names(Alka::naming(Alka::pascalCase), NameKind::Type, "player_state", "PlayerState"));
static_assert(names(Alka::naming(Alka::only<NameKind::EnumValue>(Alka::camelCase)),
                    NameKind::EnumValue, "RED_APPLE", "redApple"));

constexpr auto google =
    Alka::naming(Alka::stripSuffix<"_">, Alka::camelCase, Alka::rename<"mOSes", "osList">);
static_assert(names(google, P, "hit_points_", "hitPoints"));
static_assert(names(google, P, "mOSes", "osList"));

constexpr auto handler = Alka::naming([](const NameKind kind, const std::string_view name) {
    Alka::NameText out(name.starts_with("x_") ? name.substr(2) : name);
    if (kind == NameKind::Function) {
        out += "Fn";
    }
    return out;
});
static_assert(names(handler, NameKind::Function, "x_roll", "rollFn"));
static_assert(names(handler, P, "x_y", "y"));

class Unit {
public:
    int hit_points_ = 10;
    int max_speed_ = 3;
    std::string mHTTPSPort = "443";
    [[nodiscard]] int get_level() const { return 7; }
    static inline int unit_count_ = 2;
    static int make_id() { return 42; }
};

struct Radio {
    int mVolume = 3;
    static constexpr auto alkaNaming = Alka::memberPrefixes;
    static constexpr auto alkaMembers =
        Alka::members(Alka::ctor<>(), Alka::prop<&Radio::mVolume>());
};

struct Vehicle {
    virtual ~Vehicle() = default;
    int mWheels = 4;
};

struct Truck : Vehicle {
    int mLoad = 9;
};

struct Plain {
    int mValue = 5;
};

class CWidget {};

class Speaker {
public:
    virtual ~Speaker() = default;
    virtual std::string Speak() { return "base"; }
};

struct JsSpeaker : Speaker, Alka::Overridable<Speaker> {
    std::string Speak() override { return ALKA_OVERRIDE(std::string, Speaker, Speak); }
};

inline std::string speakOf(Speaker& speaker) {
    return speaker.Speak();
}

enum class Tint { kRed = 1, kDeepBlue = 2 };

struct Point3 {
    int pos_x = 0;
    int pos_y = 0;
};

inline int roll_dice(const int sides) {
    return sides;
}
inline Point3 nudge(Point3 p) {
    ++p.pos_x;
    return p;
}

struct Api;

} // namespace NamingTest

template <> struct Alka::Bind<NamingTest::Unit> : Class<NamingTest::Unit> {
    static constexpr auto naming =
        Alka::naming(Alka::stripSuffix<"_">, camelCase, Alka::rename<"mHTTPSPort", "httpsPort">);
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::prop<&NamingTest::Unit::hit_points_>(),
        Alka::prop<&NamingTest::Unit::max_speed_>(), Alka::prop<&NamingTest::Unit::mHTTPSPort>(),
        Alka::method<&NamingTest::Unit::get_level>(),
        Alka::staticProp<&NamingTest::Unit::unit_count_>(),
        Alka::staticMethod<&NamingTest::Unit::make_id>(),
        Alka::prop<"raw_speed", &NamingTest::Unit::max_speed_>());
};

template <> struct Alka::Bind<NamingTest::Vehicle> : Class<NamingTest::Vehicle> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&NamingTest::Vehicle::mWheels>());
};

template <> struct Alka::Bind<NamingTest::Truck> : Class<NamingTest::Truck> {
    using Extends = NamingTest::Vehicle;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&NamingTest::Truck::mLoad>());
};

template <> struct Alka::Bind<NamingTest::Plain> : Class<NamingTest::Plain> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&NamingTest::Plain::mValue>());
};

template <> struct Alka::Bind<NamingTest::CWidget> : Class<NamingTest::CWidget> {
    static constexpr auto naming = Alka::naming(Alka::only<NameKind::Type>(Alka::stripPrefix<"C">));
    static constexpr auto members = Alka::members(Alka::ctor<>());
};

template <> struct Alka::Bind<NamingTest::Speaker> : Class<NamingTest::Speaker> {
    static constexpr auto naming = Alka::naming(camelCase);
    using Trampoline = NamingTest::JsSpeaker;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"speak", &NamingTest::Speaker::Speak>());
};

template <> struct Alka::Bind<NamingTest::Point3> : Record<NamingTest::Point3> {
    static constexpr auto naming = Alka::naming(camelCase);
    static constexpr auto members = Alka::members(Alka::prop<&NamingTest::Point3::pos_x>(),
                                                  Alka::prop<&NamingTest::Point3::pos_y>());
};

template <> struct Alka::Bind<NamingTest::Api> : Module<"naming"> {
    static constexpr auto naming =
        Alka::naming(camelCase, Alka::only<NameKind::EnumValue>(Alka::stripPrefix<"k">),
                     Alka::rename<"Tint", "Hue">);
    static constexpr auto members =
        Alka::members(Alka::type<NamingTest::Unit>, Alka::type<NamingTest::Radio>,
                      Alka::type<NamingTest::Vehicle>, Alka::type<NamingTest::Truck>,
                      Alka::type<NamingTest::Plain>, Alka::type<NamingTest::CWidget>,
                      Alka::type<NamingTest::Speaker>, Alka::type<NamingTest::Point3>,
                      Alka::fn<&NamingTest::roll_dice>(), Alka::fn<&NamingTest::nudge>(),
                      Alka::fn<&NamingTest::speakOf>(), Alka::enumType<NamingTest::Tint>());
};

TEST(Naming, BindingsNameTheirMembersTheClassAndModuleMembersThroughTheirNaming) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<NamingTest::Api>(context);
    });

    EXPECT_EQ(fx.evalString("const u = new naming.Unit(); [u.hitPoints, u.maxSpeed, u.httpsPort, "
                            "u.getLevel(), naming.Unit.unitCount, naming.Unit.makeId(), "
                            "u.raw_speed].join()"),
              "10,3,443,7,2,42,3");
    EXPECT_TRUE(fx.evalBool("!('hit_points_' in u) && !('maxSpeed_' in u)"));
    EXPECT_EQ(fx.evalInt("new naming.Radio().volume"), 3);
    EXPECT_EQ(fx.evalString("const t = new naming.Truck(); [t.wheels, t.load].join()"), "4,9");
    EXPECT_EQ(fx.evalInt("new naming.Plain().mValue"), 5);
    // the class's own name
    EXPECT_EQ(fx.evalString("naming.Widget.name"), "Widget");
    EXPECT_EQ(fx.evalInt("naming.rollDice(6)"), 6);
    EXPECT_EQ(fx.evalString("[naming.Hue.red, naming.Hue.deepBlue, naming.Hue[2]].join()"),
              "1,2,deepBlue");
    EXPECT_EQ(fx.evalString("const p = naming.nudge({ posX: 1, posY: 2 }); "
                            "[p.posX, p.posY, 'pos_x' in p].join()"),
              "2,2,false");

    const auto dts = Alka::generateDts<NamingTest::Api>();
    for (const std::string_view expected :
         {"hitPoints: number;", "httpsPort: string;", "static unitCount: number;",
          "static makeId(): number;", "class Widget", "function rollDice(", "enum Hue",
          "deepBlue = 2,", "interface Point3", "posX: number;"}) {
        EXPECT_THAT(dts, testing::HasSubstr(expected)) << expected;
    }
}

TEST(Naming, AlkaOverrideFindsAJSOverrideUnderTheNameTheClassNamingGivesIt) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<NamingTest::Api>(context);
    });
    EXPECT_EQ(fx.evalString("class Loud extends naming.Speaker { speak() { return 'LOUD ' + "
                            "super.speak(); } } naming.speakOf(new Loud())"),
              "LOUD base");
    EXPECT_EQ(fx.evalString("naming.speakOf(new naming.Speaker())"), "base");
}

namespace NamingTest {

struct Clash {
    int mValue = 1;
    [[nodiscard]] int value() const { return 2; }
};

struct ReservedStatic {
    static int name() { return 1; }
};

struct ReservedMethod {
    [[nodiscard]] int build() const { return 1; }
};

enum class Shade { Red, RED };

struct ShadeMod;
struct LeftMod;
struct RightMod;
struct OtherMod;
struct SameClassMod;

inline int left() {
    return 1;
}
inline int right() {
    return 2;
}

} // namespace NamingTest

template <> struct Alka::Bind<NamingTest::Clash> : Class<NamingTest::Clash> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&NamingTest::Clash::mValue>(),
                      Alka::method<&NamingTest::Clash::value>());
};

template <> struct Alka::Bind<NamingTest::ReservedStatic> : Class<NamingTest::ReservedStatic> {
    static constexpr auto members =
        Alka::members(Alka::staticMethod<&NamingTest::ReservedStatic::name>());
};

template <> struct Alka::Bind<NamingTest::ReservedMethod> : Class<NamingTest::ReservedMethod> {
    static constexpr auto members =
        Alka::members(Alka::method<&NamingTest::ReservedMethod::build>(),
                      Alka::method<"constructor", &NamingTest::ReservedMethod::build>());
};

template <> struct Alka::Bind<NamingTest::ShadeMod> : Module<"shades"> {
    static constexpr auto naming = Alka::naming(Alka::only<NameKind::EnumValue>(camelCase));
    static constexpr auto members = Alka::members(Alka::enumType<NamingTest::Shade>());
};

template <> struct Alka::Bind<NamingTest::LeftMod> : Module<"shared"> {
    static constexpr auto members =
        Alka::members(Alka::fn<"pick", &NamingTest::left>(), Alka::type<NamingTest::Plain>);
};

template <> struct Alka::Bind<NamingTest::RightMod> : Module<"shared"> {
    static constexpr auto members = Alka::members(Alka::fn<"pick", &NamingTest::right>());
};

template <> struct Alka::Bind<NamingTest::OtherMod> : Module<"shared"> {
    static constexpr auto members = Alka::members(Alka::fn<"other", &NamingTest::right>());
};

template <> struct Alka::Bind<NamingTest::SameClassMod> : Module<"shared"> {
    static constexpr auto members = Alka::members(Alka::type<NamingTest::Plain>);
};

namespace NamingTest {

using Alka::Detail::findModuleClash;
using Alka::Detail::findNameClash;
using Alka::Detail::memberNameEntries;

static_assert(findNameClash(memberNameEntries<Clash>()).mName == "value");
static_assert(findNameClash(memberNameEntries<ReservedStatic>()).mReserved);
static_assert(findNameClash(memberNameEntries<ReservedMethod>()).mName == "constructor");
static_assert(findNameClash(Alka::Detail::enumNameEntries<ShadeMod, Shade>()).mName == "red");
static_assert(findNameClash(memberNameEntries<Unit>()).mName.empty());

static_assert(findModuleClash<LeftMod, RightMod>(false).mName == "pick");
static_assert(findModuleClash<LeftMod, OtherMod>(false).mModule == nullptr);
static_assert(findModuleClash<LeftMod, SameClassMod>(false).mModule == nullptr);
static_assert(findModuleClash<LeftMod, OtherMod>(true).mPath == "shared" &&
              findModuleClash<LeftMod, OtherMod>(true).mName.empty());

} // namespace NamingTest

TEST(Naming, ModulesInstalledOneByOneMergeRefuseANameDefinedTwiceInstallOnce) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<NamingTest::LeftMod>(context);
        Alka::install<NamingTest::LeftMod>(context);
        Alka::install<NamingTest::OtherMod, NamingTest::SameClassMod>(context);
        EXPECT_THROW(Alka::install<NamingTest::RightMod>(context), Alka::JsException);
    });
    EXPECT_EQ(fx.evalString("[shared.pick(), shared.other(), typeof shared.Plain].join()"),
              "1,2,function");

    const auto dts = Alka::generateDts<NamingTest::LeftMod, NamingTest::OtherMod>();
    EXPECT_THAT(dts, testing::HasSubstr("function pick("));
    EXPECT_THAT(dts, testing::HasSubstr("function other("));
}

TEST(Naming, NameTextHoldsANameAtRunTimeToo) {
    Alka::NameText text("abc");
    text += "def";
    EXPECT_EQ(text, "abcdef");
    for (int i = 0; i < 300; ++i) {
        text.push_back('x');
    }
    EXPECT_EQ(text.size(), Alka::NameText::kCapacity);
    EXPECT_EQ(Alka::memberPrefixes.apply(Alka::NameKind::Property, "mURLPath"), "urlPath");
}
