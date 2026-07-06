#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <doctest/doctest.h>

#include <string>

namespace MultiTest {

struct Thing {
    Thing() = default;
    int mValue = 0;
    void bump() { ++mValue; }
};

inline Thing gShared;
inline Thing* shared() { return &gShared; }

struct CoreMod;
struct ExtrasMod;

}  // namespace MultiTest

template<>
struct Alka::Bind<MultiTest::Thing> : Alka::Class<MultiTest::Thing> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&MultiTest::Thing::bump>(),
        Alka::prop<&MultiTest::Thing::mValue>());
};

template<>
struct Alka::Bind<MultiTest::CoreMod> : Alka::Module<"mc"> {
    static constexpr auto members = Alka::members(
        Alka::type<MultiTest::Thing>, Alka::fn<&MultiTest::shared>());
};

template<>
struct Alka::Bind<MultiTest::ExtrasMod> : Alka::Module<"mcExtras"> {
    static constexpr auto members = Alka::members(Alka::fn<"ping", [] { return 1; }>());
};

namespace {

v8::Local<v8::Value> evalIn(v8::Isolate* isolate, v8::Local<v8::Context> context,
                            std::string_view source) {
    const v8::Context::Scope contextScope(context);
    return AlkaTest::V8Fixture::compileAndRun(isolate, context, source).ToLocalChecked();
}

int evalIntIn(v8::Isolate* isolate, v8::Local<v8::Context> context, std::string_view source) {
    return evalIn(isolate, context, source)->Int32Value(context).FromMaybe(-999);
}

bool evalBoolIn(v8::Isolate* isolate, v8::Local<v8::Context> context, std::string_view source) {
    return evalIn(isolate, context, source)->BooleanValue(isolate);
}

}  // namespace

TEST_CASE("modules install per context, independently") {
    MultiTest::gShared.mValue = 0;
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> contextA) {
        v8::Local<v8::Context> contextB = v8::Context::New(isolate);
        Alka::install<MultiTest::CoreMod, MultiTest::ExtrasMod>(contextA);
        Alka::install<MultiTest::CoreMod>(contextB);

        // selective installation
        CHECK(evalIntIn(isolate, contextA, "mcExtras.ping()") == 1);
        CHECK(evalBoolIn(isolate, contextB, "typeof mcExtras === 'undefined'"));

        // each context constructs and dispatches through its own templates
        CHECK(evalIntIn(isolate, contextA,
                        "(() => { const t = new mc.Thing(); t.bump(); return t.value; })()") == 1);
        CHECK(evalIntIn(isolate, contextB,
                        "(() => { const t = new mc.Thing(); t.bump(); t.bump(); return t.value; })()") ==
              2);

        // identity caches are per-context...
        CHECK(evalBoolIn(isolate, contextA, "mc.shared() === mc.shared()"));
        CHECK(evalBoolIn(isolate, contextB, "mc.shared() === mc.shared()"));

        // ...but both wrappers alias the same native object
        CHECK(evalIntIn(isolate, contextA, "(() => { mc.shared().bump(); return mc.shared().value; })()") ==
              1);
        CHECK(evalIntIn(isolate, contextB, "mc.shared().value") == 1);
        CHECK(evalBoolIn(isolate, contextB, "(() => { mc.shared().bump(); return true; })()"));
        CHECK(MultiTest::gShared.mValue == 2);

        // prototype identity stays local to each context
        CHECK(evalBoolIn(isolate, contextA,
                         "Object.getPrototypeOf(new mc.Thing()) === mc.Thing.prototype"));
        CHECK(evalBoolIn(isolate, contextB,
                         "Object.getPrototypeOf(new mc.Thing()) === mc.Thing.prototype"));
    });
}
