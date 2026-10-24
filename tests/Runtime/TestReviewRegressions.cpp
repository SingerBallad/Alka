#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>
#include <Alka/Runtime/Snapshot.hpp>

#include <libplatform/libplatform.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace AlkaTest {
extern v8::Platform* gPlatform;
}

namespace RegressTest {

struct Thing {
    inline static int sAlive = 0;
    Thing() { ++sAlive; }
    ~Thing() { --sAlive; }
};

struct Pt {
    double mX = 0;
    double mY = 0;
};

struct Poly {
    std::vector<Pt> mPts;
    void add(double x) { mPts.push_back(Pt{x, 0}); }
    void clear() {
        mPts.clear();
        mPts.shrink_to_fit();
    }
    [[nodiscard]] std::size_t size() const { return mPts.size(); }
    Pt& operator[](std::size_t i) { return mPts[i]; }
};

struct Inner {
    int mX = 3;
};

struct Parent : Alka::Trackable {
    Inner mIn;
};

inline Parent* gParent = nullptr;
inline Parent* parent() {
    return gParent;
}

struct Shell {
    Inner mIn;
};
inline Shell gShell;
inline Shell* borrowShell() {
    return &gShell;
}
inline std::shared_ptr<Shell> shareShell() {
    return std::shared_ptr<Shell>(&gShell, [](Shell*) {});
}

struct Worker {
    int mN = 1;
    Alka::Task<int> work() {
        co_await Alka::offload([] {});
        co_return mN;
    }
};

inline Worker gWorker;
inline Worker* borrowedWorker() {
    return &gWorker;
}

inline int count(const std::vector<int>& v) {
    return static_cast<int>(v.size());
}

inline std::optional<Alka::Value> gForeign;
inline Alka::Value foreign() {
    return gForeign.value();
}

class Animal {
public:
    virtual ~Animal() = default;
    virtual std::string speak() { return "..."; }
};
struct JsAnimal : Animal, Alka::Overridable<Animal> {
    std::string speak() override { return ALKA_OVERRIDE(std::string, Animal, speak); }
};
inline std::shared_ptr<Animal> gKept;
inline void keep(std::shared_ptr<Animal> a) {
    gKept = std::move(a);
}
inline Animal* kept() {
    return gKept.get();
}
inline std::string speakKept() {
    return gKept->speak();
}

inline Alka::Task<int> inner(int v) {
    co_return v + 1;
}
inline Alka::Task<int> outer(int v) {
    co_await Alka::offload([] {});
    co_return co_await inner(v);
}

struct Gauge {
    int mValue = 7;
    [[nodiscard]] int doubled() const { return mValue * 2; }
};

struct FrameProbe {
    inline static std::atomic<int> sAlive{0};
    FrameProbe() { ++sAlive; }
    ~FrameProbe() { --sAlive; }
    FrameProbe(const FrameProbe&) = delete;
    FrameProbe& operator=(const FrameProbe&) = delete;
};
inline std::atomic gReleaseWorker{false};
inline std::atomic gObservedSum{0};
inline Alka::Task<int> slowLocal() {
    FrameProbe probe;
    std::vector local(1000, 1);
    const int sum = co_await Alka::offload([&local] {
        while (!gReleaseWorker.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        int s = 0;
        for (const int v : local) {
            s += v;
        }
        gObservedSum = s;
        return s;
    });
    co_return sum;
}

struct ResumeOnThread {
    bool await_ready() const noexcept { return false; }
    void await_suspend(const std::coroutine_handle<> handle) const {
        std::thread([handle] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            handle.resume();
        }).detach();
    }
    void await_resume() const noexcept {}
};
inline Alka::Task<int> finishOffThread() {
    co_await ResumeOnThread{};
    co_return 5;
}
inline std::atomic gFailOffThread{true};
inline Alka::Task<int> failOffThread() {
    co_await ResumeOnThread{};
    if (gFailOffThread) {
        throw std::runtime_error("boom off-thread");
    }
    co_return 0;
}

struct Bag {
    std::set<int> mItems{1, 2, 3};
    void clear() { mItems.clear(); }
    [[nodiscard]] auto begin() const { return mItems.begin(); }
    [[nodiscard]] auto end() const { return mItems.end(); }
};

struct Mod;

} // namespace RegressTest

template <> struct Alka::Bind<RegressTest::Thing> : Class<RegressTest::Thing> {
    static constexpr auto members = Alka::members(Alka::ctor<>());
};

template <> struct Alka::Bind<RegressTest::Pt> : Record<RegressTest::Pt> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::prop<&RegressTest::Pt::mX>(), Alka::prop<&RegressTest::Pt::mY>());
};

template <> struct Alka::Bind<RegressTest::Poly> : Class<RegressTest::Poly> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&RegressTest::Poly::add>(),
        Alka::method<&RegressTest::Poly::clear>(),
        Alka::prop<"length", &RegressTest::Poly::size>(Alka::int64As<Int64Mode::Number>),
        Alka::indexer());
};

template <> struct Alka::Bind<RegressTest::Inner> : Class<RegressTest::Inner> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&RegressTest::Inner::mX>());
};

template <> struct Alka::Bind<RegressTest::Parent> : Class<RegressTest::Parent> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&RegressTest::Parent::mIn>());
};

template <> struct Alka::Bind<RegressTest::Shell> : Class<RegressTest::Shell> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&RegressTest::Shell::mIn>());
};

template <> struct Alka::Bind<RegressTest::Worker> : Class<RegressTest::Worker> {
    static constexpr auto members = Alka::members(Alka::method<&RegressTest::Worker::work>());
};

template <> struct Alka::Bind<RegressTest::Animal> : Class<RegressTest::Animal> {
    using Trampoline = RegressTest::JsAnimal;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"speak", &RegressTest::Animal::speak>());
};

template <> struct Alka::Bind<RegressTest::Gauge> : Class<RegressTest::Gauge> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::prop<"doubled", &RegressTest::Gauge::doubled>()); // getter only
};

template <> struct Alka::Bind<RegressTest::Bag> : Class<RegressTest::Bag> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&RegressTest::Bag::clear>(),
                      Alka::iterable<RegressTest::Bag>());
};

template <> struct Alka::Bind<RegressTest::Mod> : Module<"reg"> {
    static constexpr auto members = Alka::members(
        Alka::type<RegressTest::Thing>, Alka::type<RegressTest::Pt>, Alka::type<RegressTest::Poly>,
        Alka::type<RegressTest::Inner>, Alka::type<RegressTest::Parent>,
        Alka::type<RegressTest::Worker>, Alka::type<RegressTest::Animal>,
        Alka::type<RegressTest::Gauge>, Alka::type<RegressTest::Bag>,
        Alka::type<RegressTest::Shell>, Alka::fn<&RegressTest::borrowShell>(policy::borrow),
        Alka::fn<&RegressTest::shareShell>(), Alka::fn<&RegressTest::parent>(),
        Alka::fn<&RegressTest::borrowedWorker>(policy::borrow), Alka::fn<&RegressTest::count>(),
        Alka::fn<&RegressTest::foreign>(), Alka::fn<&RegressTest::keep>(),
        Alka::fn<&RegressTest::kept>(policy::borrow), Alka::fn<&RegressTest::speakKept>(),
        Alka::fn<&RegressTest::outer>(), Alka::fn<&RegressTest::slowLocal>(),
        Alka::fn<&RegressTest::finishOffThread>(), Alka::fn<&RegressTest::failOffThread>());
};

namespace {

struct RegFixture : AlkaTest::V8Fixture {
    RegFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<RegressTest::Mod>(context);
        });
    }

    template <class Pred> bool pumpUntil(Pred done) {
        for (int i = 0; i < 5000 && !done(); ++i) {
            run([](v8::Isolate* isolate, v8::Local<v8::Context>) {
                (void)Alka::Dispatcher(isolate).drain();
                isolate->PerformMicrotaskCheckpoint();
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }
};

} // namespace

TEST(ReviewRegressions, DisposalBetweenTheTwoWeakCallbackPasses) {
    RegressTest::Thing::sAlive = 0;
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator.get();
    v8::Isolate* isolate = v8::Isolate::New(params);
    {
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        const auto context = v8::Context::New(isolate);
        const v8::Context::Scope contextScope(context);
        Alka::install<RegressTest::Mod>(context);
        (void)AlkaTest::V8Fixture::compileAndRun(
            isolate, context, "for (let i = 0; i < 200000; ++i) { new reg.Thing(); } 'ok'");
    }
    Alka::disposeIsolate(isolate);
    while (v8::platform::PumpMessageLoop(AlkaTest::gPlatform, isolate)) {
    }
    isolate->Dispose();
    EXPECT_EQ(RegressTest::Thing::sAlive, 0);
}

TEST(ReviewRegressions, IndexerWriteReChecksBoundsAfterRunningJS) {
    RegFixture fx;
    EXPECT_EQ(fx.evalString("(() => { const p = new reg.Poly(); p.add(1); p.add(2);"
                            " p[0] = { get x() { p.clear(); return 5; }, y: 0 };"
                            " return String(p.length); })()"),
              "0");
}

TEST(ReviewRegressions, FieldViewsDieWithADestroyedTrackableParent) {
    RegFixture fx;
    RegressTest::gParent = new RegressTest::Parent();
    fx.evalString("globalThis.view = reg.parent().in; 'ok'");
    EXPECT_EQ(fx.evalInt("view.x"), 3);
    delete RegressTest::gParent;
    RegressTest::gParent = nullptr;
    EXPECT_EQ(fx.evalString("try { view.x } catch (e) { e.message }"),
              "Inner.x: the native Inner object is not (or no longer) alive");
}

TEST(ReviewRegressions, FieldViewsSurviveAnOwnershipUpgradeOfTheirParent) {
    RegFixture fx;
    EXPECT_EQ(fx.evalInt("(() => { const s = reg.borrowShell(); const v = s.in;"
                         " if (reg.shareShell() !== s) return -1; /* same wrapper, now owning */"
                         " return v.x; })()"),
              3);
}

TEST(ReviewRegressions, CoroutineMethodRefusesABorrowedReceiver) {
    RegFixture fx;
    EXPECT_EQ(
        fx.evalString("try { reg.borrowedWorker().work(); 'started' } catch (e) { e.message }"),
        "Worker.work: a coroutine method needs a receiver JS owns or shares (this one is "
        "borrowed from C++ and could be destroyed while the coroutine is suspended)");
}

TEST(ReviewRegressions, HostileArrayLengthsDoNotAbortTheProcess) {
    RegFixture fx;
    EXPECT_EQ(fx.evalString("try { reg.count(new Array(2 ** 32 - 1)) } catch (e) { e.message }"),
              "arg 1 of reg.count: element 0: expected number, got undefined");
}

TEST(ReviewRegressions, HandleOfAnotherIsolateIsRefusedNotReinterpreted) {
    {
        AlkaTest::V8Fixture other;
        other.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
            Alka::ConvertCtx cx{isolate, context};
            RegressTest::gForeign.emplace(cx, v8::Object::New(isolate));
        });
        RegFixture fx;
        EXPECT_EQ(fx.evalString("try { reg.foreign(); 'converted' } catch (e) { e.message }"),
                  "an Alka handle belongs to another isolate");
        RegressTest::gForeign.reset();
    }
}

TEST(ReviewRegressions, WrappingInASecondContextKeepsTheJSOverride) {
    RegFixture fx;
    fx.evalString(
        "reg.keep(new (class extends reg.Animal { speak() { return 'woof'; } })()); 'ok'");
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context>) {
        const auto second = v8::Context::New(isolate);
        const v8::Context::Scope scope(second);
        Alka::install<RegressTest::Mod>(second);
        (void)AlkaTest::V8Fixture::compileAndRun(isolate, second, "reg.kept(); 'ok'");
    });
    EXPECT_EQ(fx.evalString("reg.speakKept()"), "woof");
    RegressTest::gKept.reset();
}

TEST(ReviewRegressions, NestedTaskAfterOffloadHasAContext) {
    RegFixture fx;
    fx.evalString(
        "globalThis.out = 'pending'; reg.outer(41).then(v => out = v, e => out = e.message); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("String(out)") != "pending"; }));
    EXPECT_EQ(fx.evalString("String(out)"), "42");
}

TEST(ReviewRegressions, SnapshotReferenceTableHandlesGetterOnlyProps) {
    EXPECT_NE(Alka::snapshot::externalReferences<RegressTest::Mod>(), nullptr);
}

TEST(ReviewRegressions, OffloadedWorkOutlivesIsolateDisposalSafely) {
    RegressTest::gReleaseWorker = false;
    RegressTest::gObservedSum = 0;
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator.get();
    v8::Isolate* isolate = v8::Isolate::New(params);
    {
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        const auto context = v8::Context::New(isolate);
        const v8::Context::Scope contextScope(context);
        Alka::install<RegressTest::Mod>(context);
        (void)AlkaTest::V8Fixture::compileAndRun(isolate, context, "reg.slowLocal(); 'ok'");
    }
    EXPECT_EQ(RegressTest::FrameProbe::sAlive, 1);
    Alka::disposeIsolate(isolate);
    isolate->Dispose();
    EXPECT_EQ(RegressTest::FrameProbe::sAlive, 1);
    RegressTest::gReleaseWorker = true;
    for (int i = 0; i < 5000 && RegressTest::FrameProbe::sAlive != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(RegressTest::gObservedSum, 1000);
    EXPECT_EQ(RegressTest::FrameProbe::sAlive, 0);
}

TEST(ReviewRegressions, TaskFinishingOnAnotherThreadSettlesOnTheIsolateThread) {
    RegFixture fx;
    fx.evalString("globalThis.a = 'pending'; globalThis.b = 'pending';"
                  " reg.finishOffThread().then(v => a = v);"
                  " reg.failOffThread().catch(e => b = e.message); 'ok'");
    EXPECT_TRUE(fx.pumpUntil(
        [&] { return fx.evalString("a + '|' + b").find("pending") == std::string::npos; }));
    EXPECT_EQ(fx.evalString("a + '|' + b"), "5|boom off-thread");
}

TEST(ReviewRegressions, SnapshotIterationSurvivesHostileArrayPrototypes) {
    RegFixture fx;
    EXPECT_EQ(
        fx.evalString("(() => { const bag = new reg.Bag();"
                      " Object.defineProperty(Array.prototype, '0', { set(v) { bag.clear(); },"
                      " configurable: true });"
                      " try { return [...bag].join(','); }"
                      " finally { delete Array.prototype[0]; } })()"),
        "1,2,3");
    EXPECT_EQ(fx.evalString("(() => { const saved = Array.prototype[Symbol.iterator];"
                            " Array.prototype[Symbol.iterator] = null;"
                            " try { for (const x of new reg.Bag()) {} return 'iterated'; }"
                            " catch (e) { return e.message; }"
                            " finally { Array.prototype[Symbol.iterator] = saved; } })()"),
              "Array.prototype[Symbol.iterator] is not a function");
}
