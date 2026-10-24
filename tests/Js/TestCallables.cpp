#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace CallTest {

#if ALKA_HAS_GENERATOR
inline std::generator<int> countTo(int n) {
    for (int i = 1; i <= n; ++i) {
        co_yield i;
    }
}

inline int gDestroyed = 0;
struct Guard {
    Guard() = default;
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() { ++gDestroyed; }
};
inline std::generator<std::string> words() {
    const Guard guard;
    co_yield "a";
    co_yield "b";
    co_yield "c";
}

inline std::generator<int> failing() {
    co_yield 1;
    throw std::runtime_error("generator broke");
}

struct Deck {
    std::vector<int> mCards{3, 1, 2};
    std::generator<int> deal() {
        for (const int card : mCards) {
            co_yield card; // (the frame uses `this`)
        }
    }
};
#endif

inline Alka::Task<std::string> slowWork(Alka::AbortSignal signal) {
    for (int i = 0; i < 20 && !signal.aborted(); ++i) {
        co_await Alka::offload([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    }
    signal.throwIfAborted();
    co_return "finished";
}

inline std::atomic gReceived{0};
inline void feed(Alka::ThreadSafeFunction<void(int)> sink, int count) {
    std::thread([sink, count] {
        for (int i = 1; i <= count; ++i) {
            sink.post(i);
        }
    }).detach();
}

struct Mod;

} // namespace CallTest

#if ALKA_HAS_GENERATOR
template <> struct Alka::Bind<CallTest::Deck> : Class<CallTest::Deck> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&CallTest::Deck::deal>());
};
#endif

template <> struct Alka::Bind<CallTest::Mod> : Module<"calls"> {
    static constexpr auto members = Alka::members(
#if ALKA_HAS_GENERATOR
        Alka::fn<&CallTest::countTo>(arg("n")), Alka::fn<&CallTest::words>(),
        Alka::fn<&CallTest::failing>(), Alka::type<CallTest::Deck>,
#endif
        Alka::fn<&CallTest::slowWork>(arg("signal")),
        Alka::fn<&CallTest::feed>(arg("sink"), arg("count")));
};

namespace {

struct CallFixture : AlkaTest::V8Fixture {
    CallFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<CallTest::Mod>(context);
        });
        evalString("globalThis.AbortController = class { constructor() { const l = [];"
                   " this.signal = { aborted: false, reason: undefined,"
                   "   addEventListener(t, f) { if (t === 'abort') l.push(f); } };"
                   " this.abort = r => { this.signal.aborted = true; this.signal.reason = r;"
                   "   for (const f of l) f.call(this.signal, {}); }; } }; 'ok'");
    }
};

} // namespace

TEST(Callables, AlkaCallableMakesJSFunctionsFromStatefulCxxCallables) {
    CallFixture fx;
    auto counter = std::make_shared<int>(0);
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto global = Alka::Object::global(cx);
        EXPECT_TRUE(global.set(
            "bump", Alka::callable([counter](int by) { return *counter += by; }, "bump")));
        EXPECT_TRUE(
            global.set("join", Alka::callable(
                                   [prefix = std::make_unique<std::string>(">")](
                                       const Alka::CallInfo& call, Alka::Rest<std::string> parts) {
                                       std::string out = *prefix;
                                       for (const auto& p : parts) {
                                           out += p;
                                       }
                                       return out + (call.mThis->IsObject() ? "" : "?");
                                   },
                                   "join")));
    });
    EXPECT_EQ(fx.evalInt("bump(2) + bump(3)"), 7);
    EXPECT_EQ(*counter, 5);
    EXPECT_EQ(fx.evalString("bump.name"), "bump");
    EXPECT_EQ(fx.evalString("join('a', 'b', 'c')"), ">abc");
    EXPECT_EQ(fx.evalString("try { bump('x') } catch (e) { e.message }"),
              "arg 1 of bump: expected number, got string");
    const auto dts =
        Alka::Detail::tsNameOf<decltype(Alka::callable([](int, Alka::Rest<int>) { return 0; }))>(
            Alka::TsNameRegistry{})
            .mText;
    EXPECT_EQ(dts, "(arg0: number, ...arg1: number[]) => number");
}

#if ALKA_HAS_GENERATOR
TEST(Callables, StdGeneratorResultsAreJSIterators) {
    CallFixture fx;
    EXPECT_EQ(fx.evalString("[...calls.countTo(4)].join(',')"), "1,2,3,4");
    EXPECT_EQ(
        fx.evalString("(() => { const it = calls.countTo(1); const a = it.next();"
                      " const b = it.next(); return a.value + ':' + a.done + '|' + b.done; })()"),
        "1:false|true");
    CallTest::gDestroyed = 0;
    EXPECT_EQ(
        fx.evalString("(() => { let first; for (const w of calls.words()) { first = w; break; }"
                      " return first; })()"),
        "a");
    EXPECT_EQ(CallTest::gDestroyed, 1);
#if !(defined(_MSC_VER) && defined(__clang__) && defined(__SANITIZE_ADDRESS__))
    EXPECT_EQ(
        fx.evalString(
            "(() => { const it = calls.failing(); it.next();"
            " try { it.next(); } catch (e) { return e.message + '/' + it.next().done; } })()"),
        "generator broke/true");
#endif
    EXPECT_EQ(
        fx.evalString(
            "(() => { const it = new calls.Deck().deal(); gc(); return [...it].join(','); })()"),
        "3,1,2");
    EXPECT_THAT(Alka::generateDts<CallTest::Mod>(),
                testing::HasSubstr("function countTo(n: number): IterableIterator<number>;"));
}
#endif

TEST(Callables, AbortSignalStopsACoroutineAndRejectsWithItsReason) {
    CallFixture fx;
    fx.evalString("globalThis.out = ''; globalThis.c = new AbortController();"
                  "calls.slowWork(c.signal).then(v => out = v, e => out = 'rejected:' + e);"
                  "c.abort('user cancelled'); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return !fx.evalString("out").empty(); }));
    EXPECT_EQ(fx.evalString("out"), "rejected:user cancelled");
    fx.evalString("globalThis.out2 = ''; calls.slowWork(new AbortController().signal)"
                  ".then(v => out2 = v); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("out2") == "finished"; }));
}

TEST(Callables, ThreadSafeFunctionIsCalledFromAnotherThreadOnTheIsolateThread) {
    CallFixture fx;
    CallTest::gReceived = 0;
    const auto mainThread = std::this_thread::get_id();
    std::atomic wrongThread{false};
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        EXPECT_TRUE(Alka::Object::global(cx).set("record", Alka::callable([&](int value) {
                                                     if (std::this_thread::get_id() != mainThread) {
                                                         wrongThread = true;
                                                     }
                                                     CallTest::gReceived += value;
                                                 })));
    });
    fx.evalString("calls.feed(record, 10); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([] { return CallTest::gReceived.load() == 55; }));
    EXPECT_FALSE(wrongThread.load());
    std::future<Alka::Result<int>> future;
    ASSERT_NO_FATAL_FAILURE(fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        v8::Local<v8::Value> fn;
        ASSERT_TRUE(
            AlkaTest::V8Fixture::compileAndRun(isolate, context, "(a, b) => a * b").ToLocal(&fn));
        const auto tsfn = Alka::fromJs<Alka::ThreadSafeFunction<int(int, int)>>(cx, fn);
        ASSERT_TRUE(tsfn);
        std::thread([copy = *tsfn, &future] { future = copy.call(6, 7); }).join();
    }));
    EXPECT_TRUE(fx.pumpUntil([&] {
        return future.valid() &&
               future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }));
    EXPECT_EQ(future.get().value_or(0), 42);
}
