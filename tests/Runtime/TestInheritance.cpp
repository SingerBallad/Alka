#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace InhTest {

struct Entity {
    Entity(std::string name, int hp) : mName(std::move(name)), mHp(hp) {}
    virtual ~Entity() = default;
    Entity(const Entity&) = default;
    Entity& operator=(const Entity&) = default;

    std::string mName;
    int mHp = 0;

    std::string describe() const { return mName + "/" + std::to_string(mHp); }
    void damage(int amount) { mHp -= amount; }
    static std::string kind() { return "entity"; }
};

struct Player : Entity {
    Player(std::string name, int level) : Entity(std::move(name), 100), mLevel(level) {}
    int mLevel = 1;
    std::string title() const { return "Lv" + std::to_string(mLevel) + " " + mName; }
};

struct Mage : Player {
    explicit Mage(std::string name) : Player(std::move(name), 10) {}
    int mMana = 99;
    int cast() const { return mMana; }
};

struct Tagged {
    virtual ~Tagged() = default;
    int mTag = 7;
};
struct Npc : Tagged, Entity {
    explicit Npc(std::string name) : Entity(std::move(name), 5) {}
    int tag() const { return mTag; }
};

struct Ghost : Tagged, Entity {
    Ghost() : Entity("ghost", 3) {}
};
inline std::unique_ptr<Entity> summon() {
    return std::make_unique<Ghost>();
}
inline Entity& haunt() {
    static Ghost ghost;
    return ghost;
}

inline std::string describeAny(const Entity& e) {
    return e.describe();
}
inline void hurt(Entity& e, int amount) {
    e.damage(amount);
}
inline Entity* promote(Player* p) {
    return p;
}
inline Entity& mirror(Entity& e) {
    return e;
}
inline Player clonePlayer(const Player& p) {
    return p;
}
inline std::unique_ptr<Entity> forge(std::string name) {
    return std::make_unique<Mage>(std::move(name));
}
inline bool isNull(Entity* e) {
    return e == nullptr;
}

struct WorldMod;

} // namespace InhTest

template <> struct Alka::Bind<InhTest::Entity> : Class<InhTest::Entity> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<std::string, int>(arg("name"), arg("hp")),
                      Alka::method<&InhTest::Entity::describe>(),
                      Alka::method<&InhTest::Entity::damage>(arg("amount")),
                      Alka::staticMethod<&InhTest::Entity::kind>(),
                      Alka::prop<&InhTest::Entity::mName>(), Alka::prop<&InhTest::Entity::mHp>());
};

template <> struct Alka::Bind<InhTest::Player> : Class<InhTest::Player> {
    static constexpr auto naming = memberPrefixes;
    using Extends = InhTest::Entity;
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(arg("name"), arg("level")),
        Alka::method<&InhTest::Player::title>(), Alka::prop<&InhTest::Player::mLevel>());
};

template <> struct Alka::Bind<InhTest::Mage> : Class<InhTest::Mage> {
    static constexpr auto naming = memberPrefixes;
    using Extends = InhTest::Player;
    static constexpr auto members =
        Alka::members(Alka::ctor<std::string>(arg("name")), Alka::method<&InhTest::Mage::cast>(),
                      Alka::prop<&InhTest::Mage::mMana>());
};

template <> struct Alka::Bind<InhTest::Npc> : Class<InhTest::Npc> {
    using Extends = InhTest::Entity;
    static constexpr auto members =
        Alka::members(Alka::ctor<std::string>(arg("name")), Alka::method<&InhTest::Npc::tag>());
};

template <> struct Alka::Bind<InhTest::WorldMod> : Module<"world"> {
    static constexpr auto members = Alka::members(
        Alka::type<InhTest::Entity>, Alka::type<InhTest::Player>, Alka::type<InhTest::Mage>,
        Alka::type<InhTest::Npc>, Alka::fn<&InhTest::describeAny>(arg("e")),
        Alka::fn<&InhTest::hurt>(arg("e"), arg("amount")),
        Alka::fn<&InhTest::promote>(arg("p"), policy::borrow),
        Alka::fn<&InhTest::mirror>(arg("e"), policy::borrow),
        Alka::fn<&InhTest::clonePlayer>(arg("p")), Alka::fn<&InhTest::forge>(arg("name")),
        Alka::fn<&InhTest::isNull>(arg("e")), Alka::fn<&InhTest::summon>(),
        Alka::fn<&InhTest::haunt>(policy::borrow));
};

namespace {

struct WorldFixture : AlkaTest::V8Fixture {
    WorldFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<InhTest::WorldMod>(context);
        });
    }
};

} // namespace

TEST(Inheritance, PrototypeChainMirrorsTheCxxHierarchy) {
    WorldFixture fx;
    fx.evalString("globalThis.m = new world.Mage('gandalf'); 'ok'");
    EXPECT_TRUE(fx.evalBool("m instanceof world.Mage"));
    EXPECT_TRUE(fx.evalBool("m instanceof world.Player"));
    EXPECT_TRUE(fx.evalBool("m instanceof world.Entity"));
    EXPECT_TRUE(
        fx.evalBool("Object.getPrototypeOf(world.Mage.prototype) === world.Player.prototype"));
    EXPECT_TRUE(
        fx.evalBool("Object.getPrototypeOf(world.Player.prototype) === world.Entity.prototype"));
}

TEST(Inheritance, DerivedInstancesUseBaseMethodsAndProps) {
    WorldFixture fx;
    fx.evalString("globalThis.m = new world.Mage('gandalf'); 'ok'");
    EXPECT_EQ(fx.evalString("m.describe()"), "gandalf/100");
    EXPECT_EQ(fx.evalString("m.title()"), "Lv10 gandalf");
    EXPECT_EQ(fx.evalInt("m.cast()"), 99);
    fx.evalString("m.damage(30); 'ok'");
    EXPECT_EQ(fx.evalInt("m.hp"), 70);
    EXPECT_EQ(fx.evalString("m.name"), "gandalf");
}

TEST(Inheritance, StaticsAreInheritedLikeJSClassStatics) {
    WorldFixture fx;
    EXPECT_EQ(fx.evalString("world.Entity.kind()"), "entity");
    EXPECT_EQ(fx.evalString("world.Mage.kind()"), "entity");
}

TEST(Inheritance, PassingDerivedWhereBaseIsExpectedWithAliasing) {
    WorldFixture fx;
    fx.evalString("globalThis.m = new world.Mage('gandalf'); 'ok'");
    EXPECT_EQ(fx.evalString("world.describeAny(m)"), "gandalf/100");
    fx.evalString("world.hurt(m, 40); 'ok'");
    EXPECT_EQ(fx.evalInt("m.hp"), 60);
}

TEST(Inheritance, NonFirstBaseUpcastAdjustsThePointer) {
    WorldFixture fx;
    fx.evalString("globalThis.n = new world.Npc('bob'); 'ok'");
    EXPECT_EQ(fx.evalInt("n.tag()"), 7);
    EXPECT_EQ(fx.evalString("world.describeAny(n)"), "bob/5");
    fx.evalString("world.hurt(n, 1); 'ok'");
    EXPECT_EQ(fx.evalString("n.describe()"), "bob/4");
}

TEST(Inheritance, UnboundMostDerivedTypeIsReachedThroughItsBoundBase) {
    WorldFixture fx;
    EXPECT_EQ(fx.evalString("world.summon().describe()"), "ghost/3");
    EXPECT_EQ(fx.evalString("world.haunt().describe()"), "ghost/3");
    EXPECT_TRUE(fx.evalBool("world.haunt() === world.haunt()")); // identity still holds
    EXPECT_TRUE(fx.evalBool("world.summon() instanceof world.Entity"));
    EXPECT_EQ(fx.evalString(
                  "(() => { const g = world.haunt(); world.hurt(g, 1); return g.describe(); })()"),
              "ghost/2");
}

TEST(Inheritance, UpcastingInTheWrongDirectionIsRejected) {
    WorldFixture fx;
    EXPECT_EQ(
        fx.evalString("try { world.promote(new world.Entity('e', 1)) } catch (e) { e.message }"),
        "arg 1 (p) of world.promote: expected Player, got Entity");
    EXPECT_EQ(fx.evalString("try { world.describeAny({}) } catch (e) { e.message }"),
              "arg 1 (e) of world.describeAny: expected Entity, got a plain JS object");
}

TEST(Inheritance, PolymorphicDownTypingBaseReturnsSurfaceAsTheDerivedClass) {
    WorldFixture fx;
    fx.evalString("globalThis.p = new world.Player('conan', 3); 'ok'");
    fx.evalString("globalThis.e = world.promote(p); 'ok'");
    EXPECT_TRUE(fx.evalBool("e instanceof world.Player"));
    EXPECT_EQ(fx.evalString("e.title()"), "Lv3 conan");
    fx.evalString("globalThis.f = world.forge('saruman'); 'ok'");
    EXPECT_TRUE(fx.evalBool("f instanceof world.Mage"));
    EXPECT_EQ(fx.evalInt("f.cast()"), 99);
}

TEST(Inheritance, ReferenceReturnsAliasTheSameNativeObject) {
    WorldFixture fx;
    fx.evalString("globalThis.m = new world.Mage('gandalf'); 'ok'");
    fx.evalString("globalThis.r = world.mirror(m); 'ok'");
    EXPECT_TRUE(fx.evalBool("r instanceof world.Mage"));
    fx.evalString("r.damage(10); 'ok'");
    EXPECT_EQ(fx.evalInt("m.hp"), 90);
}

TEST(Inheritance, ValueReturnsCopyCopiesAreIndependent) {
    WorldFixture fx;
    fx.evalString("globalThis.p = new world.Player('conan', 3); 'ok'");
    fx.evalString("globalThis.c = world.clonePlayer(p); 'ok'");
    fx.evalString("c.damage(50); 'ok'");
    EXPECT_EQ(fx.evalInt("c.hp"), 50);
    EXPECT_EQ(fx.evalInt("p.hp"), 100);
}

TEST(Inheritance, NullPointersCrossTheBoundary) {
    WorldFixture fx;
    EXPECT_TRUE(fx.evalBool("world.isNull(null)"));
    EXPECT_TRUE(fx.evalBool("world.isNull(undefined)"));
    EXPECT_TRUE(fx.evalBool("!world.isNull(new world.Entity('e', 1))"));
}

TEST(Inheritance, JSClassesExtendNativeClasses) {
    WorldFixture fx;
    fx.evalString(R"js(
        globalThis.Boss = class Boss extends world.Player {
            constructor(name) { super(name, 99); this.rage = 5; }
            taunt() { return this.title() + '!!'; }
            describeTwice() { return super.describe() + super.describe(); }
        };
        globalThis.b = new Boss('demon'); 'ok'
    )js");
    EXPECT_TRUE(fx.evalBool("b instanceof Boss"));
    EXPECT_TRUE(fx.evalBool("b instanceof world.Player"));
    EXPECT_TRUE(fx.evalBool("b instanceof world.Entity"));
    EXPECT_EQ(fx.evalInt("b.rage"), 5);
    EXPECT_EQ(fx.evalString("b.title()"), "Lv99 demon");
    EXPECT_EQ(fx.evalString("b.taunt()"), "Lv99 demon!!");
    EXPECT_EQ(fx.evalString("b.describeTwice()"), "demon/100demon/100");
    EXPECT_EQ(fx.evalString("world.describeAny(b)"), "demon/100");
    fx.evalString("world.hurt(b, 10); 'ok'");
    EXPECT_EQ(fx.evalInt("b.hp"), 90);
}

TEST(Inheritance, JSMethodOverridesShadowForJSCallsButNotForCxxCalls) {
    WorldFixture fx;
    fx.evalString(R"js(
        class Sneak extends world.Entity {
            constructor() { super('sneak', 1); }
            describe() { return 'js:' + super.describe(); }
        }
        globalThis.s = new Sneak(); 'ok'
    )js");
    EXPECT_EQ(fx.evalString("s.describe()"), "js:sneak/1");
    EXPECT_EQ(fx.evalString("world.describeAny(s)"), "sneak/1");
}
