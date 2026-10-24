#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace LifetimeTest {

struct Tracked {
    inline static int sAlive = 0;
    inline static int sDestroyed = 0;
    static void reset() {
        sAlive = 0;
        sDestroyed = 0;
    }

    Tracked() { ++sAlive; }
    ~Tracked() {
        --sAlive;
        ++sDestroyed;
    }

    int ping() const { return 1; }
};

struct SharedThing {
    inline static int sAlive = 0;
    SharedThing() { ++sAlive; }
    ~SharedThing() { --sAlive; }
    int mValue = 5;
};

inline std::shared_ptr<SharedThing> gKeep;

inline std::shared_ptr<SharedThing> makeShared() {
    return std::make_shared<SharedThing>();
}
inline int useCount(const std::shared_ptr<SharedThing>& p) {
    return static_cast<int>(p.use_count());
}
inline void keep(std::shared_ptr<SharedThing> p) {
    gKeep = std::move(p);
}
inline void dropKeep() {
    gKeep.reset();
}

struct LifeMod;

} // namespace LifetimeTest

template <> struct Alka::Bind<LifetimeTest::Tracked> : Class<LifetimeTest::Tracked> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&LifetimeTest::Tracked::ping>());
};

template <> struct Alka::Bind<LifetimeTest::SharedThing> : Class<LifetimeTest::SharedThing> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&LifetimeTest::SharedThing::mValue>());
};

template <> struct Alka::Bind<LifetimeTest::LifeMod> : Module<"life"> {
    static constexpr auto members =
        Alka::members(Alka::type<LifetimeTest::Tracked>, Alka::type<LifetimeTest::SharedThing>,
                      Alka::fn<"same", [](LifetimeTest::Tracked* t) { return t; }>(policy::borrow),
                      Alka::fn<&LifetimeTest::makeShared>(), Alka::fn<&LifetimeTest::useCount>(),
                      Alka::fn<&LifetimeTest::keep>(), Alka::fn<&LifetimeTest::dropKeep>());
};

TEST(Lifetime, JSOwnedObjectsAreDestroyedByGC) {
    LifetimeTest::Tracked::reset();
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });

    EXPECT_EQ(fx.evalInt("{ let t = new life.Tracked(); t.ping() }"), 1);
    EXPECT_EQ(LifetimeTest::Tracked::sAlive, 1);

    fx.collectGarbage();
    EXPECT_EQ(LifetimeTest::Tracked::sAlive, 0);
    EXPECT_EQ(LifetimeTest::Tracked::sDestroyed, 1);
}

TEST(Lifetime, LiveObjectsSurviveGCWhileReferenced) {
    LifetimeTest::Tracked::reset();
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });

    fx.evalString("globalThis.keep = new life.Tracked(); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(LifetimeTest::Tracked::sAlive, 1);
    EXPECT_EQ(fx.evalInt("keep.ping()"), 1);

    fx.evalString("globalThis.keep = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(LifetimeTest::Tracked::sAlive, 0);
}

TEST(Lifetime, IdentityCacheTheSameNativeObjectIsTheSameJSObject) {
    LifetimeTest::Tracked::reset();
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });

    EXPECT_TRUE(
        fx.evalBool("(() => { const t = new life.Tracked(); return life.same(t) === t; })()"));
    EXPECT_TRUE(fx.evalBool("(() => { const t = new life.Tracked();"
                            " return life.same(t) === life.same(t); })()"));
    EXPECT_EQ(fx.evalInt("(() => { const t = new life.Tracked(); t.custom = 7;"
                         " return life.same(t).custom; })()"),
              7);
}

TEST(Lifetime, SharedPtrCrossesTheBoundaryAndSharesOwnership) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });

    fx.evalString("globalThis.s = life.makeShared(); 'ok'");
    EXPECT_EQ(LifetimeTest::SharedThing::sAlive, 1);
    EXPECT_EQ(fx.evalInt("s.value"), 5);
    EXPECT_GE(fx.evalInt("life.useCount(s)"), 1);
    EXPECT_TRUE(fx.evalBool("life.makeShared() !== s"));

    fx.evalString("life.keep(s); globalThis.s = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(LifetimeTest::SharedThing::sAlive, 1);
    LifetimeTest::dropKeep();
    fx.collectGarbage();
    EXPECT_EQ(LifetimeTest::SharedThing::sAlive, 0);

    fx.evalString("globalThis.t = life.makeShared(); 'ok'");
    EXPECT_EQ(LifetimeTest::SharedThing::sAlive, 1);
    fx.evalString("globalThis.t = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(LifetimeTest::SharedThing::sAlive, 0);
}

TEST(Lifetime, NonSharedWrappersRefuseSharedPtrRestoration) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });
    EXPECT_EQ(fx.evalString("try { life.useCount(new life.Tracked()) } catch (e) { e.message }"),
              "arg 1 of life.useCount: expected SharedThing, got Tracked");
}

TEST(Lifetime, DisposeIsolateSweepsObjectsTheGCNeverCollected) {
    LifetimeTest::Tracked::reset();
    {
        AlkaTest::V8Fixture fx;
        fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<LifetimeTest::LifeMod>(context);
        });
        fx.evalString("globalThis.a = new life.Tracked();"
                      "globalThis.b = new life.Tracked();"
                      "globalThis.c = new life.Tracked(); 'ok'");
        EXPECT_EQ(LifetimeTest::Tracked::sAlive, 3);
    }
    EXPECT_EQ(LifetimeTest::Tracked::sAlive, 0);
    EXPECT_EQ(LifetimeTest::Tracked::sDestroyed, 3);
}
