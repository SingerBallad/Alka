#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ToolsTest {

struct Enemy : Alka::Trackable {
    int mHp = 10;
};

inline Enemy* gEnemy = nullptr;
inline Enemy* enemy() {
    return gEnemy;
}

struct Blob {
    inline static int sAlive = 0;
    inline static int sDestroyedWhileJsUsable = 0;
    explicit Blob(int kb) : mBytes(static_cast<std::size_t>(kb) * 1024) { ++sAlive; }
    ~Blob() { --sAlive; }
    std::size_t mBytes;
    [[nodiscard]] int kb() const { return static_cast<int>(mBytes / 1024); }
    void grow(int kb) { mBytes += static_cast<std::size_t>(kb) * 1024; }
};

struct Button {
    inline static int sAlive = 0;
    Button() { ++sAlive; }
    ~Button() { --sAlive; }
    Button(const Button&) = delete;
    Button& operator=(const Button&) = delete;
    Alka::Retained<Alka::Function<int()>> mOnClick;
    int click() const { return mOnClick ? mOnClick.get().call() : -1; }
    bool held() const { return mOnClick.weak(); }
    void watch(Alka::Function<int()> callback) { mOnClick.set(this, callback); }
};

inline Button* gLoose = nullptr;
inline Button* loose() {
    if (gLoose == nullptr) {
        gLoose = new Button();
    }
    return gLoose;
}
inline std::unique_ptr<Button> adoptLoose() {
    return std::unique_ptr<Button>(std::exchange(gLoose, nullptr));
}

inline std::shared_ptr<Button> gShared;
inline std::shared_ptr<Button> sharedButton() {
    if (!gShared) {
        gShared = std::make_shared<Button>();
    }
    return gShared;
}
inline void dropShared() {
    gShared.reset();
}

inline Button* gRemembered = nullptr;
inline void remember(Button* b) {
    gRemembered = b;
}
inline Button* remembered() {
    return gRemembered;
}

struct Emitter {
    void on(const Alka::Value&) {}
    void clear(Alka::CallInfo call) { Alka::releaseKept(call); }
};
inline void releaseEmitter(v8::Isolate* isolate, Emitter* e) {
    Alka::releaseKept(isolate, e);
}

inline std::vector<std::shared_ptr<Button>> gPanel;
inline void addToPanel(std::shared_ptr<Button> b) {
    gPanel.push_back(std::move(b));
}

inline Button gStatic;
inline Button* staticButton() {
    return &gStatic;
}

struct Mod;

} // namespace ToolsTest

template <> struct Alka::Bind<ToolsTest::Enemy> : Class<ToolsTest::Enemy> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&ToolsTest::Enemy::mHp>());
};

template <> struct Alka::Bind<ToolsTest::Blob> : Class<ToolsTest::Blob> {
    static std::size_t externalSize(const ToolsTest::Blob& blob) { return blob.mBytes; }
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(), Alka::method<&ToolsTest::Blob::kb>(),
                      Alka::method<&ToolsTest::Blob::grow>());
};

template <> struct Alka::Bind<ToolsTest::Button> : Class<ToolsTest::Button> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::prop<&ToolsTest::Button::mOnClick>(),
        Alka::method<&ToolsTest::Button::click>(), Alka::method<&ToolsTest::Button::held>(),
        Alka::method<&ToolsTest::Button::watch>());
};

template <> struct Alka::Bind<ToolsTest::Emitter> : Class<ToolsTest::Emitter> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&ToolsTest::Emitter::on>(Alka::keepAlive<0>),
                      Alka::method<&ToolsTest::Emitter::clear>());
};

template <> struct Alka::Bind<ToolsTest::Mod> : Module<"tools"> {
    static constexpr auto members = Alka::members(
        Alka::type<ToolsTest::Enemy>, Alka::type<ToolsTest::Blob>, Alka::type<ToolsTest::Button>,
        Alka::type<ToolsTest::Emitter>, Alka::fn<&ToolsTest::enemy>(),
        Alka::fn<&ToolsTest::addToPanel>(), Alka::fn<&ToolsTest::staticButton>(policy::borrow),
        Alka::fn<"noop", [] {}>(), Alka::fn<&ToolsTest::loose>(policy::borrow),
        Alka::fn<&ToolsTest::adoptLoose>(), Alka::fn<&ToolsTest::sharedButton>(),
        Alka::fn<&ToolsTest::dropShared>(), Alka::fn<&ToolsTest::remember>(),
        Alka::fn<&ToolsTest::remembered>(policy::borrow), Alka::fn<&ToolsTest::releaseEmitter>());
};

namespace {

struct ToolsFixture : AlkaTest::V8Fixture {
    ToolsFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<ToolsTest::Mod>(context);
        });
    }

    [[nodiscard]] std::int64_t externalMemory() { return Alka::reportedExternalMemory(isolate()); }
};

} // namespace

TEST(LifetimeTools, TrackableObjectsDetachTheirBorrowedWrappersWhenDestroyedOnAnyThread) {
    ToolsFixture fx;
    ToolsTest::gEnemy = new ToolsTest::Enemy();
    fx.evalString("globalThis.e = tools.enemy(); 'ok'");
    EXPECT_EQ(fx.evalInt("e.hp"), 10);

    std::thread([] { delete ToolsTest::gEnemy; }).join();
    ToolsTest::gEnemy = nullptr;
    EXPECT_EQ(fx.evalString("try { e.hp } catch (err) { err.message }"),
              "Enemy.hp: the native Enemy object is not (or no longer) alive");

    ToolsTest::gEnemy = new ToolsTest::Enemy();
    EXPECT_TRUE(fx.evalBool("tools.enemy() !== e && tools.enemy().hp === 10"));
    delete ToolsTest::gEnemy;
    ToolsTest::gEnemy = nullptr;
}

TEST(LifetimeTools, DeferredDestructionRunsDestructorsAtTheHostSafePoint) {
    ToolsFixture fx;
    Alka::setDestructionMode(fx.isolate(), Alka::DestructionMode::Deferred);
    const int before = ToolsTest::Blob::sAlive;
    fx.evalString("{ new tools.Blob(1); new tools.Blob(2); } 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Blob::sAlive, before + 2);
    EXPECT_EQ(Alka::runDeferredDestruction(fx.isolate(), 1), 1);
    EXPECT_EQ(ToolsTest::Blob::sAlive, before + 1);
    EXPECT_EQ(Alka::runDeferredDestruction(fx.isolate()), 1);
    EXPECT_EQ(ToolsTest::Blob::sAlive, before);
    Alka::setDestructionMode(fx.isolate(), Alka::DestructionMode::Immediate);
}

TEST(LifetimeTools, JSOwnedObjectsReportTheirNativeMemoryToV8) {
    ToolsFixture fx;
    const auto base = fx.externalMemory();
    fx.evalString("globalThis.b = new tools.Blob(512); 'ok'");
    EXPECT_EQ(fx.externalMemory(), base + 512 * 1024);
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        const auto value =
            AlkaTest::V8Fixture::compileAndRun(isolate, context, "b").ToLocalChecked();
        Alka::ConvertCtx cx{isolate, context};
        auto* blob = Alka::fromJs<ToolsTest::Blob*>(cx, value).value();
        blob->grow(512);
        Alka::updateExternalSize(isolate, blob);
    });
    EXPECT_EQ(fx.externalMemory(), base + 1024 * 1024);
    fx.evalString("globalThis.b = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(fx.externalMemory(), base);
}

TEST(LifetimeTools, CxxSharePinsTheJSWrapperIdentityAndJSStateSurvive) {
    ToolsFixture fx;
    ToolsTest::gPanel.clear();
    fx.evalString("{ const b = new tools.Button(); b.tag = 'kept'; tools.addToPanel(b);"
                  " globalThis.firstId = b; } globalThis.firstId = null; 'ok'");
    fx.collectGarbage();
    ASSERT_EQ(ToolsTest::gPanel.size(), 1);
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto js = Alka::toJs(cx, Alka::borrow(ToolsTest::gPanel[0].get()));
        std::ignore = context->Global()->Set(context, Alka::Detail::v8Str(isolate, "again"), js);
    });
    EXPECT_EQ(fx.evalString("again.tag"), "kept");

    const int before = ToolsTest::Button::sAlive;
    ToolsTest::gPanel.clear();
    fx.evalString("globalThis.again = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Button::sAlive, before - 1);
}

TEST(LifetimeTools, RetainedBreaksTheClosureWrapperCxxClosureCycle) {
    ToolsFixture fx;
    const int before = ToolsTest::Button::sAlive;
    fx.evalString("{ const b = new tools.Button(); b.count = 0;"
                  "  b.onClick = () => ++b.count;"
                  "  globalThis.result = [b.click(), b.click(), b.held()].join(','); } 'ok'");
    EXPECT_EQ(fx.evalString("result"), "1,2,true");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Button::sAlive, before);
}

TEST(LifetimeTools, RetainedValuesSurviveAsLongAsCxxSharesTheOwner) {
    ToolsFixture fx;
    ToolsTest::gPanel.clear();
    fx.evalString(
        "{ const b = new tools.Button(); b.onClick = () => 42; tools.addToPanel(b); } 'ok'");
    fx.collectGarbage();
    ASSERT_EQ(ToolsTest::gPanel.size(), 1);
    int clicked = 0;
    fx.run([&](v8::Isolate*, v8::Local<v8::Context>) { clicked = ToolsTest::gPanel[0]->click(); });
    EXPECT_EQ(clicked, 42);
    ToolsTest::gPanel.clear();
}

TEST(LifetimeTools, RetainedOnABorrowedOwnerHoldsTheValueStrongly) {
    ToolsFixture fx;
    fx.evalString("tools.staticButton().onClick = () => 7; 'ok'");
    fx.collectGarbage();
    EXPECT_TRUE(fx.evalBool("tools.staticButton().held() === false"));
    EXPECT_EQ(fx.evalInt("tools.staticButton().click()"), 7);
    EXPECT_EQ(fx.evalInt("tools.staticButton().onClick()"), 7);
    fx.evalString("tools.staticButton().onClick = null; 'ok'");
    EXPECT_EQ(fx.evalInt("tools.staticButton().click()"), -1);
}

TEST(LifetimeTools, RetainedSetThisValueFromCxxIsAsCycleSafeAsTheProperty) {
    ToolsFixture fx;
    const int before = ToolsTest::Button::sAlive;
    fx.evalString("{ const b = new tools.Button(); b.count = 0; b.watch(() => ++b.count);"
                  "  globalThis.result = [b.click(), b.click(), b.held()].join(','); } 'ok'");
    EXPECT_EQ(fx.evalString("result"), "1,2,true");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Button::sAlive, before);
}

TEST(LifetimeTools, RetainedTurnsWeakWhenJSComesToOwnItsOwner) {
    ToolsFixture fx;
    const int before = ToolsTest::Button::sAlive;
    fx.evalString("{ const b = tools.loose(); b.onClick = () => b ? 1 : 0;"
                  "  const strong = b.held(); const same = tools.adoptLoose() === b;"
                  "  globalThis.result = [strong, same, b.held(), b.click()].join(','); } 'ok'");
    EXPECT_EQ(fx.evalString("result"), "false,true,true,1");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Button::sAlive, before);
    fx.evalString("{ const b = tools.sharedButton(); b.onClick = () => b ? 2 : 0;"
                  "  const strong = b.held(); tools.dropShared();"
                  "  globalThis.result = [strong, b.click(), b.held()].join(','); } 'ok'");
    EXPECT_EQ(fx.evalString("result"), "false,2,true");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Button::sAlive, before);
}

TEST(LifetimeTools, RetainedValuesFromAnotherContextLiveOnTheOwningWrapper) {
    ToolsFixture fx;
    const int before = ToolsTest::Button::sAlive;
    fx.evalString("globalThis.owned = new tools.Button(); tools.remember(owned); 'ok'");
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context>) {
        const auto second = v8::Context::New(isolate);
        const v8::Context::Scope scope(second);
        Alka::install<ToolsTest::Mod>(second);
        v8::Local<v8::Value> held;
        ASSERT_TRUE(
            AlkaTest::V8Fixture::compileAndRun(
                isolate, second, "const b = tools.remembered(); b.onClick = () => 99; b.held()")
                .ToLocal(&held));
        EXPECT_TRUE(held->IsTrue());
    });
    EXPECT_EQ(fx.evalInt("owned.click()"), 99);
    fx.evalString("globalThis.owned = null; 'ok'");
    ToolsTest::gRemembered = nullptr;
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Button::sAlive, before);
}

TEST(LifetimeTools, ReleaseKeptDropsWhatKeepAliveIKept) {
    ToolsFixture fx;
    const int before = ToolsTest::Blob::sAlive;
    fx.evalString("globalThis.e = new tools.Emitter();"
                  " e.on(new tools.Blob(1)); e.on(new tools.Blob(1)); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Blob::sAlive, before + 2);
    fx.evalString("e.clear(); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Blob::sAlive, before);
    fx.evalString("e.on(new tools.Blob(1)); tools.releaseEmitter(e); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(ToolsTest::Blob::sAlive, before);
}

TEST(LifetimeTools, RetainedPropertyTypesRenderAsTheirInnerType) {
    const auto dts = Alka::generateDts<ToolsTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("onClick: () => number;"));
}
