#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <expected>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ErrorTest {

struct GameError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct NotFound : GameError {
    using GameError::GameError;
    int mCode = 404;
};

struct Unregistered : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Racy : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Late : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void throwNotFound() {
    throw NotFound("missing thing");
}
inline void throwRacy() {
    throw Racy("racy");
}
inline void throwLate() {
    throw Late("late");
}
inline void throwGame() {
    throw GameError("game over");
}
inline void throwPlain() {
    throw Unregistered("plain");
}
inline std::expected<int, NotFound> lookup(int id) {
    if (id < 0) {
        return std::unexpected(NotFound("no id " + std::to_string(id)));
    }
    return id;
}
inline Alka::Task<int> lookupLater(int id) {
    co_await Alka::offload([] {});
    if (id < 0) {
        throw NotFound("later");
    }
    co_return id;
}

struct Mod;

struct Opts {
    int mX = 0;
};
struct Box {
    Opts mOpts;
};
inline int takeOpts(Opts o) {
    return o.mX;
}
inline int takeInts(const std::vector<int>& v) {
    return static_cast<int>(v.size());
}
inline int takeStrings(const std::vector<std::string>& v) {
    return static_cast<int>(v.size());
}
inline int takeMap(const std::map<std::string, int>& m) {
    return static_cast<int>(m.size());
}
inline int takePair(std::pair<int, int> p) {
    return p.first + p.second;
}
inline int takeVariant(const std::variant<int, Opts>& v) {
    return static_cast<int>(v.index());
}
inline int pickOpts(Opts) {
    return 1;
}
inline int pickInts(const std::vector<int>&) {
    return 2;
}

struct ConvMod;

} // namespace ErrorTest

template <> struct Alka::Bind<ErrorTest::Opts> : Record<ErrorTest::Opts> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&ErrorTest::Opts::mX>());
};

template <> struct Alka::Bind<ErrorTest::Box> : Class<ErrorTest::Box> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&ErrorTest::Box::mOpts>());
};

template <> struct Alka::Bind<ErrorTest::ConvMod> : Module<"conv"> {
    static constexpr auto members =
        Alka::members(Alka::type<ErrorTest::Opts>, Alka::type<ErrorTest::Box>,
                      Alka::fn<&ErrorTest::takeOpts>(), Alka::fn<&ErrorTest::takeInts>(),
                      Alka::fn<&ErrorTest::takeStrings>(), Alka::fn<&ErrorTest::takeMap>(),
                      Alka::fn<&ErrorTest::takePair>(), Alka::fn<&ErrorTest::takeVariant>(),
                      Alka::fn<"pick", &ErrorTest::pickOpts, &ErrorTest::pickInts>());
};

template <> struct Alka::Bind<ErrorTest::Mod> : Module<"errs"> {
    static constexpr auto members =
        Alka::members(Alka::error<"GameError", ErrorTest::GameError>(doc("any game failure")),
                      Alka::error<"NotFound", ErrorTest::NotFound, ErrorTest::GameError>(),
                      Alka::fn<&ErrorTest::throwNotFound>(), Alka::fn<&ErrorTest::throwGame>(),
                      Alka::fn<&ErrorTest::throwPlain>(), Alka::fn<&ErrorTest::lookup>(arg("id")),
                      Alka::fn<&ErrorTest::lookupLater>(arg("id")),
                      Alka::fn<&ErrorTest::throwRacy>(), Alka::fn<&ErrorTest::throwLate>());
};

namespace {

struct ErrorFixture : AlkaTest::V8Fixture {
    ErrorFixture() {
        Alka::registerError<ErrorTest::NotFound, ErrorTest::GameError>(
            "NotFound", [](const ErrorTest::NotFound& e, Alka::Object& error) {
                (void)error.set("code", e.mCode);
            });
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<ErrorTest::Mod>(context);
        });
    }
};

} // namespace

TEST(Errors, RegisteredCxxExceptionsSurfaceAsTheirJSErrorClasses) {
    ErrorFixture fx;
    EXPECT_TRUE(fx.evalBool(
        "(() => { try { errs.throwNotFound(); } catch (e) {"
        " return e instanceof errs.NotFound && e instanceof errs.GameError &&"
        " e instanceof Error && e.name === 'NotFound' && e.message === 'missing thing' &&"
        " e.code === 404 && typeof e.stack === 'string' &&"
        " String(e) === 'NotFound: missing thing'; } return false; })()"));
    EXPECT_TRUE(
        fx.evalBool("(() => { try { errs.throwGame(); } catch (e) {"
                    " return e instanceof errs.GameError && !(e instanceof errs.NotFound); }"
                    " return false; })()"));
    EXPECT_TRUE(fx.evalBool(
        "(() => { try { errs.throwPlain(); } catch (e) {"
        " return e.constructor === Error && e.message === 'plain'; } return false; })()"));
    EXPECT_TRUE(fx.evalBool("new errs.NotFound('made in JS') instanceof errs.GameError"));
}

TEST(Errors, ExpectedErrorsAndCoroutineFailuresUseTheRegisteredClassesToo) {
    ErrorFixture fx;
    EXPECT_EQ(fx.evalInt("errs.lookup(3)"), 3);
    EXPECT_TRUE(fx.evalBool("(() => { try { errs.lookup(-1); } catch (e) {"
                            " return e instanceof errs.NotFound && e.message === 'no id -1'; }"
                            " return false; })()"));
    fx.evalString(
        "globalThis.out = 'pending';"
        "errs.lookupLater(-1).catch(e => out = e instanceof errs.NotFound && e.code); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("String(out)") != "pending"; }));
    EXPECT_EQ(fx.evalString("String(out)"), "404");
}

TEST(Errors, ErrorTypeFirstRegistrationSticksADecoratorAttachesOnce) {
    ErrorFixture fx;
    Alka::registerError<ErrorTest::Late>("Late");
    Alka::registerError<ErrorTest::Late>("Renamed", [](const ErrorTest::Late&, Alka::Object& e) {
        (void)e.set("by", std::string("first"));
    });
    Alka::registerError<ErrorTest::Late>("Renamed", [](const ErrorTest::Late&, Alka::Object& e) {
        (void)e.set("by", std::string("second"));
    });
    EXPECT_EQ(fx.evalString("(() => { try { errs.throwLate(); } catch (e) {"
                            " return e.name + ':' + e.by; } })()"),
              "Late:first");
}

TEST(Errors, RegisteringErrorTypesFromOtherThreadsWhileOneIsThrownIsSafe) {
    ErrorFixture fx;
    std::atomic stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&stop] {
            while (!stop.load()) {
                Alka::registerError<ErrorTest::Racy>("Racy",
                                                     [](const ErrorTest::Racy&, Alka::Object& e) {
                                                         (void)e.set("decorated", true);
                                                     });
            }
        });
    }
    int recognized = 0;
    for (int i = 0; i < 300; ++i) {
        recognized += fx.evalBool("(() => { try { errs.throwRacy(); } catch (e) {"
                                  " return e.message === 'racy'; } })()")
                          ? 1
                          : 0;
    }
    stop = true;
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(recognized, 300);
    EXPECT_TRUE(fx.evalBool("(() => { try { errs.throwRacy(); } catch (e) {"
                            " return e.name === 'Racy' && e.decorated === true; } })()"));
}

TEST(Errors, JSThatThrowsWhileAnArgumentConvertsPropagatesItsOwnException) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<ErrorTest::ConvMod>(context);
    });
    fx.evalString(R"(
        globalThis.boom = new Error('boom');
        globalThis.reads = 0;
        globalThis.bad = { get x() { ++reads; throw boom; } };
        globalThis.withBadElement = (values) => {
            const array = [...values];
            Object.defineProperty(array, 1, { get() { throw boom; } });
            return array;
        };
        globalThis.same = (f) => {
            try { f(); return 'no throw'; } catch (e) { return e === boom ? 'same' : String(e); }
        };
        'ok')");
    EXPECT_EQ(fx.evalString("same(() => conv.takeOpts(bad))"), "same");
    EXPECT_EQ(fx.evalString("same(() => conv.takeInts(withBadElement([1, 2, 3])))"), "same");
    EXPECT_EQ(fx.evalString("same(() => conv.takeStrings(withBadElement(['a', 'b'])))"), "same");
    EXPECT_EQ(fx.evalString("same(() => conv.takeMap({ a: 1, get b() { throw boom; } }))"), "same");
    EXPECT_EQ(
        fx.evalString("same(() => conv.takeMap(new Proxy({}, { ownKeys() { throw boom; } })))"),
        "same");
    EXPECT_EQ(fx.evalString("same(() => conv.takePair(withBadElement([1, 2])))"), "same");
    EXPECT_EQ(fx.evalString("same(() => conv.takeVariant(bad))"), "same");
    EXPECT_EQ(fx.evalString("same(() => { new conv.Box().opts = bad; })"), "same");
    EXPECT_EQ(fx.evalString("reads = 0; same(() => conv.pick(bad)) + ':' + reads"), "same:1");
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        v8::Local<v8::Value> list;
        ASSERT_TRUE(
            AlkaTest::V8Fixture::compileAndRun(isolate, context, "[{ x: 1 }, bad]").ToLocal(&list));
        const auto converted = Alka::fromJs<std::vector<ErrorTest::Opts>>(cx, list);
        ASSERT_FALSE(converted);
        EXPECT_THAT(converted.error().message(),
                    testing::StartsWith("element 1: key 'x': Error: boom"));
        v8::Local<v8::Value> boom;
        ASSERT_TRUE(AlkaTest::V8Fixture::compileAndRun(isolate, context, "boom").ToLocal(&boom));
        const auto thrown = Alka::exceptionValue(converted.error());
        EXPECT_TRUE(thrown.valid() && thrown.local(isolate)->StrictEquals(boom));
    });
    EXPECT_EQ(fx.evalString("same(() => conv.takeOpts({ x: 'no' }))"),
              "TypeError: arg 1 of conv.takeOpts: key 'x': expected number, got string");
}

TEST(Errors, ErrorClassesInTheDts) {
    const auto dts = Alka::generateDts<ErrorTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("class GameError extends Error {}"));
    EXPECT_THAT(dts, testing::HasSubstr("class NotFound extends errs.GameError {}"));
}
