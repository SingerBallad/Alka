#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace OwnTest {

struct Box {
    inline static int sAlive = 0;
    Box() { ++sAlive; }
    Box(const Box&) : Box() {}
    ~Box() { --sAlive; }
    int mValue = 5;
};

struct Inner {
    int mX = 3;
};

struct Outer {
    inline static int sAlive = 0;
    Outer() { ++sAlive; }
    ~Outer() { --sAlive; }
    Inner mIn;
    Inner& in() { return mIn; }
};

inline std::shared_ptr<Box> gShared;
inline Box* gRaw = nullptr;
inline Outer gOuter;

inline Box* borrowShared() {
    return gShared.get();
}
inline std::shared_ptr<Box> shareShared() {
    return gShared;
}
inline Box* borrowRaw() {
    return gRaw;
}
inline std::unique_ptr<Box> releaseRaw() {
    std::unique_ptr<Box> owned(gRaw);
    gRaw = nullptr;
    return owned;
}
inline Outer* outer() {
    return &gOuter;
}

struct Mod;

} // namespace OwnTest

template <> struct Alka::Bind<OwnTest::Box> : Class<OwnTest::Box> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto lifetime = Lifetime::Manual;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&OwnTest::Box::mValue>());
};

template <> struct Alka::Bind<OwnTest::Inner> : Class<OwnTest::Inner> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&OwnTest::Inner::mX>());
};

template <> struct Alka::Bind<OwnTest::Outer> : Class<OwnTest::Outer> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&OwnTest::Outer::mIn>(),
                      Alka::method<"inRef", &OwnTest::Outer::in>(policy::internalRef),
                      Alka::method<"inLoose", &OwnTest::Outer::in>(policy::borrow));
};

template <> struct Alka::Bind<OwnTest::Mod> : Module<"own"> {
    static constexpr auto members =
        Alka::members(Alka::type<OwnTest::Box>, Alka::type<OwnTest::Inner>,
                      Alka::type<OwnTest::Outer>, Alka::fn<&OwnTest::borrowShared>(),
                      Alka::fn<&OwnTest::shareShared>(), Alka::fn<&OwnTest::borrowRaw>(),
                      Alka::fn<&OwnTest::releaseRaw>(), Alka::fn<&OwnTest::outer>(policy::borrow));
};

namespace {

struct OwnFixture : AlkaTest::V8Fixture {
    OwnFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<OwnTest::Mod>(context);
        });
    }
};

} // namespace

TEST(Ownership, ContextAlkaInstalledIntoIsStillGarbageCollected) {
    OwnTest::Box::sAlive = 0;
    AlkaTest::V8Fixture fx;
    v8::Global<v8::Context> weak;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context>) {
        const v8::HandleScope scope(isolate);
        const auto context = v8::Context::New(isolate);
        {
            const v8::Context::Scope contextScope(context);
            Alka::install<OwnTest::Mod>(context);
            Alka::registerModules<OwnTest::Mod>(context);
            EXPECT_FALSE(
                Alka::resolveModule(context, v8::String::NewFromUtf8Literal(isolate, "own"))
                    .IsEmpty());
            (void)AlkaTest::V8Fixture::compileAndRun(
                isolate, context, "globalThis.b = new own.Box(); own.outer().in; 1");
        }
        weak.Reset(isolate, context);
        weak.SetWeak();
    });
    EXPECT_EQ(OwnTest::Box::sAlive, 1);
    for (int i = 0; i < 3; ++i) {
        fx.isolate()->RequestGarbageCollectionForTesting(v8::Isolate::kFullGarbageCollection);
    }
    EXPECT_TRUE(weak.IsEmpty());
    EXPECT_EQ(OwnTest::Box::sAlive, 0);
    fx.run(
        [](v8::Isolate*, v8::Local<v8::Context> context) { Alka::install<OwnTest::Mod>(context); });
    EXPECT_EQ(fx.evalInt("new own.Box().value"), 5);
}

TEST(Ownership, ReturningASharedPtrUpgradesAnEarlierBorrowedWrapper) {
    OwnTest::Box::sAlive = 0;
    OwnFixture fx;
    OwnTest::gShared = std::make_shared<OwnTest::Box>();
    EXPECT_TRUE(fx.evalBool("globalThis.b = own.borrowShared(); globalThis.s = own.shareShared();"
                            " b === s"));
    OwnTest::gShared.reset();
    EXPECT_EQ(OwnTest::Box::sAlive, 1); // JS co-owns it now
    EXPECT_EQ(fx.evalInt("b.value"), 5);
    fx.evalString("globalThis.b = null; globalThis.s = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Box::sAlive, 0);
}

TEST(Ownership, ReturningAUniquePtrHandsOwnershipToTheExistingBorrowedWrapper) {
    OwnTest::Box::sAlive = 0;
    OwnFixture fx;
    OwnTest::gRaw = new OwnTest::Box();
    EXPECT_TRUE(fx.evalBool(
        "globalThis.b = own.borrowRaw(); b.tag = 1;"
        " (() => { const owned = own.releaseRaw(); return owned === b && owned.tag === 1; })()"));
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Box::sAlive, 1);
    fx.evalString("globalThis.b = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Box::sAlive, 0);
}

TEST(Ownership, HostCodeHandsJSAUniquePtrByMovingItIn) {
    OwnTest::Box::sAlive = 0;
    OwnFixture fx;
    static_assert(!Alka::ToJsConvertible<std::unique_ptr<OwnTest::Box>>);
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        auto box = std::make_unique<OwnTest::Box>();
        box->mValue = 9;
        context->Global()
            ->Set(context, v8::String::NewFromUtf8Literal(isolate, "moved"),
                  Alka::toJs(cx, std::move(box)))
            .Check();
        EXPECT_TRUE(Alka::toJs(cx, std::unique_ptr<OwnTest::Box>())->IsNull());
    });
    EXPECT_EQ(fx.evalInt("moved.value"), 9);
    EXPECT_EQ(OwnTest::Box::sAlive, 1);
    fx.evalString("globalThis.moved = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Box::sAlive, 0); // JS owned it
}

TEST(Ownership, FirstMemberSubobjectNeverAliasesItsParentWrapper) {
    OwnFixture fx;
    EXPECT_TRUE(fx.evalBool("(() => { const o = own.outer(); o.tag = 1; const i = own.outer().in;"
                            " return own.outer() === o && o.tag === 1 && i !== o &&"
                            " i instanceof own.Inner && o.in === i; })()"));
}

TEST(Ownership, FieldWrappersKeepTheirParentAlive) {
    OwnFixture fx;
    const int before = OwnTest::Outer::sAlive;
    fx.evalString("globalThis.inner = new own.Outer().in; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Outer::sAlive, before + 1);
    EXPECT_EQ(fx.evalInt("inner.x"), 3);
    fx.evalString("globalThis.inner = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Outer::sAlive, before);
}

TEST(Ownership, InternalRefMethodsKeepTheirObjectAlivePlainOnesDoNot) {
    OwnFixture fx;
    const int before = OwnTest::Outer::sAlive;
    fx.evalString("globalThis.a = new own.Outer().inRef(); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Outer::sAlive, before + 1);
    EXPECT_EQ(fx.evalInt("a.x"), 3);
    fx.evalString("globalThis.a = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Outer::sAlive, before);

    fx.evalString("globalThis.a = new own.Outer().inLoose(); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(OwnTest::Outer::sAlive, before);
}

TEST(Ownership, InvalidateDetachesBorrowedWrappersOfADestroyedObject) {
    OwnFixture fx;
    OwnTest::gRaw = new OwnTest::Box();
    fx.evalString("globalThis.b = own.borrowRaw(); 'ok'");
    EXPECT_EQ(fx.evalInt("b.value"), 5);

    Alka::invalidate(fx.isolate(), OwnTest::gRaw);
    delete OwnTest::gRaw;
    OwnTest::gRaw = nullptr;
    EXPECT_EQ(fx.evalString("try { b.value } catch (e) { e.message }"),
              "Box.value: the native Box object is not (or no longer) alive");

    OwnTest::gRaw = new OwnTest::Box();
    EXPECT_TRUE(fx.evalBool("own.borrowRaw() !== b && own.borrowRaw().value === 5"));
    Alka::invalidate(fx.isolate(), OwnTest::gRaw);
    delete OwnTest::gRaw;
    OwnTest::gRaw = nullptr;
}
