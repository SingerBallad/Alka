#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <doctest/doctest.h>

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

inline std::shared_ptr<SharedThing> makeShared() { return std::make_shared<SharedThing>(); }
inline int useCount(const std::shared_ptr<SharedThing>& p) {
    return static_cast<int>(p.use_count());
}
inline void keep(std::shared_ptr<SharedThing> p) { gKeep = std::move(p); }
inline void dropKeep() { gKeep.reset(); }

struct LifeMod;

}  // namespace LifetimeTest

template<>
struct Alka::Bind<LifetimeTest::Tracked> : Alka::Class<LifetimeTest::Tracked> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&LifetimeTest::Tracked::ping>());
};

template<>
struct Alka::Bind<LifetimeTest::SharedThing> : Alka::Class<LifetimeTest::SharedThing> {
    static constexpr auto members =
        Alka::members(Alka::prop<&LifetimeTest::SharedThing::mValue>());
};

template<>
struct Alka::Bind<LifetimeTest::LifeMod> : Alka::Module<"life"> {
    static constexpr auto members = Alka::members(
        Alka::type<LifetimeTest::Tracked>, Alka::type<LifetimeTest::SharedThing>,
        Alka::fn<"same", [](LifetimeTest::Tracked* t) { return t; }>(),
        Alka::fn<&LifetimeTest::makeShared>(), Alka::fn<&LifetimeTest::useCount>(),
        Alka::fn<&LifetimeTest::keep>(), Alka::fn<&LifetimeTest::dropKeep>());
};

TEST_CASE("JS-owned objects are destroyed by GC") {
    LifetimeTest::Tracked::reset();
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });

    CHECK(fx.evalInt("{ let t = new life.Tracked(); t.ping() }") == 1);
    CHECK(LifetimeTest::Tracked::sAlive == 1);

    fx.collectGarbage();
    CHECK(LifetimeTest::Tracked::sAlive == 0);
    CHECK(LifetimeTest::Tracked::sDestroyed == 1);
}

TEST_CASE("live objects survive GC while referenced") {
    LifetimeTest::Tracked::reset();
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });

    fx.evalString("globalThis.keep = new life.Tracked(); 'ok'");
    fx.collectGarbage();
    CHECK(LifetimeTest::Tracked::sAlive == 1);
    CHECK(fx.evalInt("keep.ping()") == 1);

    fx.evalString("globalThis.keep = null; 'ok'");
    fx.collectGarbage();
    CHECK(LifetimeTest::Tracked::sAlive == 0);
}

TEST_CASE("identity cache: the same native object is the same JS object") {
    LifetimeTest::Tracked::reset();
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });

    CHECK(fx.evalBool("(() => { const t = new life.Tracked(); return life.same(t) === t; })()"));
    CHECK(fx.evalBool("(() => { const t = new life.Tracked();"
                      " return life.same(t) === life.same(t); })()"));
    // JS-side expando properties survive the roundtrip because it IS the object
    CHECK(fx.evalInt("(() => { const t = new life.Tracked(); t.custom = 7;"
                     " return life.same(t).custom; })()") == 7);
}

TEST_CASE("shared_ptr crosses the boundary and shares ownership") {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });

    fx.evalString("globalThis.s = life.makeShared(); 'ok'");
    CHECK(LifetimeTest::SharedThing::sAlive == 1);
    CHECK(fx.evalInt("s.value") == 5);
    // one ref in the holder + one in the aliased parameter copy
    CHECK(fx.evalInt("life.useCount(s)") == 2);
    // identity holds for shared wrappers too
    CHECK(fx.evalBool("life.makeShared() !== s"));

    // C++ keeps it alive across a JS GC
    fx.evalString("life.keep(s); globalThis.s = null; 'ok'");
    fx.collectGarbage();
    CHECK(LifetimeTest::SharedThing::sAlive == 1);
    LifetimeTest::dropKeep();
    CHECK(LifetimeTest::SharedThing::sAlive == 0);

    // JS alone keeps it alive until collected
    fx.evalString("globalThis.t = life.makeShared(); 'ok'");
    CHECK(LifetimeTest::SharedThing::sAlive == 1);
    fx.evalString("globalThis.t = null; 'ok'");
    fx.collectGarbage();
    CHECK(LifetimeTest::SharedThing::sAlive == 0);
}

TEST_CASE("non-shared wrappers refuse shared_ptr restoration") {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<LifetimeTest::LifeMod>(context);
    });
    CHECK(fx.evalString("try { life.useCount(new life.Tracked()) } catch (e) { e.message }") ==
          "arg 1 of life.useCount: expected SharedThing, got Tracked");
}

TEST_CASE("disposeIsolate sweeps objects the GC never collected") {
    LifetimeTest::Tracked::reset();
    {
        AlkaTest::V8Fixture fx;
        fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<LifetimeTest::LifeMod>(context);
        });
        fx.evalString("globalThis.a = new life.Tracked();"
                      "globalThis.b = new life.Tracked();"
                      "globalThis.c = new life.Tracked(); 'ok'");
        CHECK(LifetimeTest::Tracked::sAlive == 3);
        // fixture dtor calls Alka::disposeIsolate before Isolate::Dispose
    }
    CHECK(LifetimeTest::Tracked::sAlive == 0);
    CHECK(LifetimeTest::Tracked::sDestroyed == 3);
}
