#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <string>

namespace MultiTest {

struct Thing {
    Thing() = default;
    int mValue = 0;
    void bump() { ++mValue; }
};

inline Thing gShared;
inline Thing* shared() {
    return &gShared;
}

struct CoreMod;
struct ExtrasMod;

} // namespace MultiTest

template <> struct Alka::Bind<MultiTest::Thing> : Class<MultiTest::Thing> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto lifetime = Lifetime::Static;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&MultiTest::Thing::bump>(),
                      Alka::prop<&MultiTest::Thing::mValue>());
};

template <> struct Alka::Bind<MultiTest::CoreMod> : Module<"mc"> {
    static constexpr auto members =
        Alka::members(Alka::type<MultiTest::Thing>, Alka::fn<&MultiTest::shared>());
};

template <> struct Alka::Bind<MultiTest::ExtrasMod> : Module<"mcExtras"> {
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

} // namespace

TEST(MultiContext, ModulesInstallPerContextIndependently) {
    MultiTest::gShared.mValue = 0;
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> contextA) {
        v8::Local<v8::Context> contextB = v8::Context::New(isolate);
        Alka::install<MultiTest::CoreMod, MultiTest::ExtrasMod>(contextA);
        Alka::install<MultiTest::CoreMod>(contextB);

        EXPECT_EQ(evalIntIn(isolate, contextA, "mcExtras.ping()"), 1);
        EXPECT_TRUE(evalBoolIn(isolate, contextB, "typeof mcExtras === 'undefined'"));

        EXPECT_EQ(evalIntIn(isolate, contextA,
                            "(() => { const t = new mc.Thing(); t.bump(); return t.value; })()"),
                  1);
        EXPECT_EQ(
            evalIntIn(
                isolate, contextB,
                "(() => { const t = new mc.Thing(); t.bump(); t.bump(); return t.value; })()"),
            2);

        EXPECT_TRUE(evalBoolIn(isolate, contextA, "mc.shared() === mc.shared()"));
        EXPECT_TRUE(evalBoolIn(isolate, contextB, "mc.shared() === mc.shared()"));

        EXPECT_EQ(evalIntIn(isolate, contextA,
                            "(() => { mc.shared().bump(); return mc.shared().value; })()"),
                  1);
        EXPECT_EQ(evalIntIn(isolate, contextB, "mc.shared().value"), 1);
        EXPECT_TRUE(
            evalBoolIn(isolate, contextB, "(() => { mc.shared().bump(); return true; })()"));
        EXPECT_EQ(MultiTest::gShared.mValue, 2);

        EXPECT_TRUE(evalBoolIn(isolate, contextA,
                               "Object.getPrototypeOf(new mc.Thing()) === mc.Thing.prototype"));
        EXPECT_TRUE(evalBoolIn(isolate, contextB,
                               "Object.getPrototypeOf(new mc.Thing()) === mc.Thing.prototype"));
    });
}

TEST(MultiContext, AlkaContextDataKeepsClearOfTheHostEmbedderData) {
    MultiTest::gShared.mValue = 0;
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context>) {
        static const char marker = 0;
        struct HostData {
            const void* mFirst = nullptr;
        };
        static HostData hostData{&marker};
        const auto context = v8::Context::New(isolate);
        const auto hostValue = v8::String::NewFromUtf8Literal(isolate, "host");
        context->SetEmbedderData(ALKA_CONTEXT_EMBEDDER_SLOT, hostValue);
        Alka::Detail::setContextPointer(context, ALKA_CONTEXT_EMBEDDER_SLOT + 1, &hostData,
                                        ALKA_EMBEDDER_TAG_CONTEXT);
        context->SetEmbedderData(ALKA_CONTEXT_EMBEDDER_SLOT + 20, v8::True(isolate));

        Alka::install<MultiTest::CoreMod>(context);
        EXPECT_TRUE(evalBoolIn(isolate, context, "mc.shared() === mc.shared()"));
        EXPECT_EQ(evalIntIn(isolate, context, "mc.shared().bump(), mc.shared().value"), 1);
        const v8::Context::Scope contextScope(context);
        EXPECT_TRUE(context->GetEmbedderData(ALKA_CONTEXT_EMBEDDER_SLOT)->StrictEquals(hostValue));
        EXPECT_EQ(Alka::Detail::contextPointer(isolate, context, ALKA_CONTEXT_EMBEDDER_SLOT + 1,
                                               ALKA_EMBEDDER_TAG_CONTEXT),
                  &hostData);
        EXPECT_TRUE(context->GetEmbedderData(ALKA_CONTEXT_EMBEDDER_SLOT + 20)->IsTrue());
    });
}
