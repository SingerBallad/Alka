#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ConcTest {

struct ConcError : std::runtime_error {
    using std::runtime_error::runtime_error;
    int mCode = 7;
};

struct Counter {
    explicit Counter(int start) : mValue(start) {}
    int bump(int by) { return mValue += by; }
    int mValue = 0;
};

struct Pair {
    int mLeft = 1;
    int mRight = 2;
};

inline int fail(int code) {
    if (code % 2 == 0) {
        throw ConcError("even " + std::to_string(code));
    }
    return code;
}
inline Pair makePair() {
    return {};
}

struct Mod;

} // namespace ConcTest

template <> struct Alka::Bind<ConcTest::Counter> : Class<ConcTest::Counter> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(), Alka::method<&ConcTest::Counter::bump>(),
                      Alka::prop<&ConcTest::Counter::mValue>());
};

template <> struct Alka::Bind<ConcTest::Pair> : Record<ConcTest::Pair> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::prop<&ConcTest::Pair::mLeft>(), Alka::prop<&ConcTest::Pair::mRight>());
};

template <> struct Alka::Bind<ConcTest::Mod> : Module<"conc"> {
    static constexpr auto members =
        Alka::members(Alka::type<ConcTest::Counter>, Alka::type<ConcTest::Pair>,
                      Alka::error<"ConcError", ConcTest::ConcError>(), Alka::fn<&ConcTest::fail>(),
                      Alka::fn<&ConcTest::makePair>());
};

TEST(Concurrency, IsolatesOnSeveralThreadsRunBoundCodeAtTheSameTime) {
    constexpr int kThreads = 4;
    constexpr int kRounds = 3;
    std::atomic good{0};
    std::atomic bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&good, &bad, t] {
            for (int round = 0; round < kRounds; ++round) {
                AlkaTest::V8Fixture fx;
                Alka::registerError<ConcTest::ConcError>(
                    "ConcError", [](const ConcTest::ConcError& e, Alka::Object& error) {
                        std::ignore = error.set("code", e.mCode);
                    });
                fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
                    Alka::install<ConcTest::Mod>(context);
                });
                const bool ok =
                    fx.evalInt("let s = 0; for (let i = 0; i < 2000; ++i) {"
                               " const c = new conc.Counter(i); s += c.bump(1) - c.value; } s") ==
                        0 &&
                    fx.evalInt("let n = 0; for (let i = 0; i < 200; ++i) {"
                               " try { n += conc.fail(i); } catch (e) {"
                               " if (e instanceof conc.ConcError && e.code === 7) n -= 1; } } n") ==
                        100 * 100 - 100 &&
                    fx.evalInt(
                        "(() => { const p = conc.makePair(); return p.left + p.right; })()") == 3 &&
                    fx.evalString("String(" + std::to_string(t) + ")") == std::to_string(t);
                fx.collectGarbage();
                (ok ? good : bad).fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(good.load(), kThreads * kRounds);
}
