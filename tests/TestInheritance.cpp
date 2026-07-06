#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <doctest/doctest.h>

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

// Non-first base: Entity sits at a nonzero offset inside Npc.
struct Tagged {
    virtual ~Tagged() = default;
    int mTag = 7;
};
struct Npc : Tagged, Entity {
    explicit Npc(std::string name) : Entity(std::move(name), 5) {}
    int tag() const { return mTag; }
};

inline std::string describeAny(const Entity& e) { return e.describe(); }
inline void hurt(Entity& e, int amount) { e.damage(amount); }
inline Entity* promote(Player* p) { return p; }  // static type Base*, dynamic type Derived
inline Entity& mirror(Entity& e) { return e; }
inline Player clonePlayer(const Player& p) { return p; }
inline std::unique_ptr<Entity> forge(std::string name) {
    return std::make_unique<Mage>(std::move(name));
}
inline bool isNull(Entity* e) { return e == nullptr; }

struct WorldMod;

}  // namespace InhTest

template<>
struct Alka::Bind<InhTest::Entity> : Alka::Class<InhTest::Entity> {
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("hp")),
        Alka::method<&InhTest::Entity::describe>(),
        Alka::method<&InhTest::Entity::damage>(Alka::arg("amount")),
        Alka::staticMethod<&InhTest::Entity::kind>(),
        Alka::prop<&InhTest::Entity::mName>(),
        Alka::prop<&InhTest::Entity::mHp>());
};

template<>
struct Alka::Bind<InhTest::Player> : Alka::Class<InhTest::Player> {
    using Extends = InhTest::Entity;
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("level")),
        Alka::method<&InhTest::Player::title>(),
        Alka::prop<&InhTest::Player::mLevel>());
};

template<>
struct Alka::Bind<InhTest::Mage> : Alka::Class<InhTest::Mage> {
    using Extends = InhTest::Player;
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string>(Alka::arg("name")),
        Alka::method<&InhTest::Mage::cast>(),
        Alka::prop<&InhTest::Mage::mMana>());
};

template<>
struct Alka::Bind<InhTest::Npc> : Alka::Class<InhTest::Npc> {
    using Extends = InhTest::Entity;
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string>(Alka::arg("name")), Alka::method<&InhTest::Npc::tag>());
};

template<>
struct Alka::Bind<InhTest::WorldMod> : Alka::Module<"world"> {
    static constexpr auto members = Alka::members(
        Alka::type<InhTest::Entity>, Alka::type<InhTest::Player>, Alka::type<InhTest::Mage>,
        Alka::type<InhTest::Npc>,
        Alka::fn<&InhTest::describeAny>(Alka::arg("e")),
        Alka::fn<&InhTest::hurt>(Alka::arg("e"), Alka::arg("amount")),
        Alka::fn<&InhTest::promote>(Alka::arg("p")),
        Alka::fn<&InhTest::mirror>(Alka::arg("e")),
        Alka::fn<&InhTest::clonePlayer>(Alka::arg("p")),
        Alka::fn<&InhTest::forge>(Alka::arg("name")),
        Alka::fn<&InhTest::isNull>(Alka::arg("e")));
};

namespace {

struct WorldFixture : AlkaTest::V8Fixture {
    WorldFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<InhTest::WorldMod>(context);
        });
    }
};

}  // namespace

TEST_CASE("prototype chain mirrors the C++ hierarchy") {
    WorldFixture fx;
    fx.evalString("globalThis.m = new world.Mage('gandalf'); 'ok'");
    CHECK(fx.evalBool("m instanceof world.Mage"));
    CHECK(fx.evalBool("m instanceof world.Player"));
    CHECK(fx.evalBool("m instanceof world.Entity"));
    CHECK(fx.evalBool(
        "Object.getPrototypeOf(world.Mage.prototype) === world.Player.prototype"));
    CHECK(fx.evalBool(
        "Object.getPrototypeOf(world.Player.prototype) === world.Entity.prototype"));
}

TEST_CASE("derived instances use base methods and props") {
    WorldFixture fx;
    fx.evalString("globalThis.m = new world.Mage('gandalf'); 'ok'");
    CHECK(fx.evalString("m.describe()") == "gandalf/100");
    CHECK(fx.evalString("m.title()") == "Lv10 gandalf");
    CHECK(fx.evalInt("m.cast()") == 99);
    fx.evalString("m.damage(30); 'ok'");
    CHECK(fx.evalInt("m.hp") == 70);
    CHECK(fx.evalString("m.name") == "gandalf");
}

TEST_CASE("statics are inherited like JS class statics") {
    WorldFixture fx;
    CHECK(fx.evalString("world.Entity.kind()") == "entity");
    CHECK(fx.evalString("world.Mage.kind()") == "entity");
}

TEST_CASE("passing derived where base is expected, with aliasing") {
    WorldFixture fx;
    fx.evalString("globalThis.m = new world.Mage('gandalf'); 'ok'");
    CHECK(fx.evalString("world.describeAny(m)") == "gandalf/100");
    fx.evalString("world.hurt(m, 40); 'ok'");
    CHECK(fx.evalInt("m.hp") == 60);  // the same native object was mutated
}

TEST_CASE("non-first base upcast adjusts the pointer") {
    WorldFixture fx;
    fx.evalString("globalThis.n = new world.Npc('bob'); 'ok'");
    CHECK(fx.evalInt("n.tag()") == 7);
    CHECK(fx.evalString("world.describeAny(n)") == "bob/5");
    fx.evalString("world.hurt(n, 1); 'ok'");
    CHECK(fx.evalString("n.describe()") == "bob/4");
}

TEST_CASE("upcasting in the wrong direction is rejected") {
    WorldFixture fx;
    CHECK(fx.evalString("try { world.promote(new world.Entity('e', 1)) } catch (e) { e.message }") ==
          "arg 1 (p) of world.promote: expected Player, got Entity");
    CHECK(fx.evalString("try { world.describeAny({}) } catch (e) { e.message }") ==
          "arg 1 (e) of world.describeAny: expected Entity, got a plain JS object");
}

TEST_CASE("polymorphic down-typing: Base* returns surface as the derived class") {
    WorldFixture fx;
    fx.evalString("globalThis.p = new world.Player('conan', 3); 'ok'");
    fx.evalString("globalThis.e = world.promote(p); 'ok'");  // C++ returns Entity*
    CHECK(fx.evalBool("e instanceof world.Player"));
    CHECK(fx.evalString("e.title()") == "Lv3 conan");
    // unique_ptr<Entity> holding a Mage surfaces as Mage
    fx.evalString("globalThis.f = world.forge('saruman'); 'ok'");
    CHECK(fx.evalBool("f instanceof world.Mage"));
    CHECK(fx.evalInt("f.cast()") == 99);
}

TEST_CASE("reference returns alias the same native object") {
    WorldFixture fx;
    fx.evalString("globalThis.m = new world.Mage('gandalf'); 'ok'");
    fx.evalString("globalThis.r = world.mirror(m); 'ok'");
    CHECK(fx.evalBool("r instanceof world.Mage"));  // down-typed too
    fx.evalString("r.damage(10); 'ok'");
    CHECK(fx.evalInt("m.hp") == 90);
}

TEST_CASE("value returns copy; copies are independent") {
    WorldFixture fx;
    fx.evalString("globalThis.p = new world.Player('conan', 3); 'ok'");
    fx.evalString("globalThis.c = world.clonePlayer(p); 'ok'");
    fx.evalString("c.damage(50); 'ok'");
    CHECK(fx.evalInt("c.hp") == 50);
    CHECK(fx.evalInt("p.hp") == 100);
}

TEST_CASE("null pointers cross the boundary") {
    WorldFixture fx;
    CHECK(fx.evalBool("world.isNull(null)"));
    CHECK(fx.evalBool("world.isNull(undefined)"));
    CHECK(fx.evalBool("!world.isNull(new world.Entity('e', 1))"));
}

TEST_CASE("JS classes extend native classes") {
    WorldFixture fx;
    fx.evalString(R"js(
        globalThis.Boss = class Boss extends world.Player {
            constructor(name) { super(name, 99); this.rage = 5; }
            taunt() { return this.title() + '!!'; }
            describeTwice() { return super.describe() + super.describe(); }
        };
        globalThis.b = new Boss('demon'); 'ok'
    )js");
    CHECK(fx.evalBool("b instanceof Boss"));
    CHECK(fx.evalBool("b instanceof world.Player"));
    CHECK(fx.evalBool("b instanceof world.Entity"));
    CHECK(fx.evalInt("b.rage") == 5);                          // JS-side state works
    CHECK(fx.evalString("b.title()") == "Lv99 demon");         // native method
    CHECK(fx.evalString("b.taunt()") == "Lv99 demon!!");       // JS method calling native
    CHECK(fx.evalString("b.describeTwice()") == "demon/100demon/100");  // super.*
    CHECK(fx.evalString("world.describeAny(b)") == "demon/100");        // passes into C++
    fx.evalString("world.hurt(b, 10); 'ok'");
    CHECK(fx.evalInt("b.hp") == 90);
}

TEST_CASE("JS method overrides shadow for JS calls but not for C++ calls") {
    WorldFixture fx;
    fx.evalString(R"js(
        class Sneak extends world.Entity {
            constructor() { super('sneak', 1); }
            describe() { return 'js:' + super.describe(); }
        }
        globalThis.s = new Sneak(); 'ok'
    )js");
    CHECK(fx.evalString("s.describe()") == "js:sneak/1");        // JS dispatch
    CHECK(fx.evalString("world.describeAny(s)") == "sneak/1");   // C++ calls the native one
}
