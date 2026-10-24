#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace DisposeTest {

struct Meta {
    int mSize = 3;
};

struct File {
    inline static int sClosed = 0;
    inline static int sDestroyed = 0;

    explicit File(std::string name) : mName(std::move(name)) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { ++sDestroyed; }

    void close() {
        mOpen = false;
        ++sClosed;
    }

    void each(const Alka::Function<void()>& callback) {
        callback.call();
        mTouched = mOpen ? 1 : 2;
    }
    Alka::Task<std::string> slowName() {
        co_await Alka::offload([] {});
        co_return mName;
    }

    std::string mName;
    bool mOpen = true;
    int mTouched = 0;
    Meta mMeta;
};

inline std::shared_ptr<File> gKept;
inline void keep(std::shared_ptr<File> file) {
    gKept = std::move(file);
}
inline std::string keptName() {
    return gKept ? gKept->mName + (gKept->mOpen ? "" : " (closed)") : "none";
}

inline File gGlobal("global");
inline File* global() {
    return &gGlobal;
}

inline void reset() {
    File::sClosed = 0;
    File::sDestroyed = 0;
    gKept.reset();
    gGlobal.mOpen = true;
}

struct Mod;

} // namespace DisposeTest

template <> struct Alka::Bind<DisposeTest::Meta> : Class<DisposeTest::Meta> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&DisposeTest::Meta::mSize>());
};

template <> struct Alka::Bind<DisposeTest::File> : Class<DisposeTest::File> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto lifetime = Lifetime::Static;
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string>(), Alka::prop<&DisposeTest::File::mName>(),
        Alka::prop<&DisposeTest::File::mOpen>(readonly),
        Alka::prop<&DisposeTest::File::mTouched>(readonly), Alka::prop<&DisposeTest::File::mMeta>(),
        Alka::method<&DisposeTest::File::each>(), Alka::method<&DisposeTest::File::slowName>(),
        Alka::disposable<&DisposeTest::File::close>());
};

template <> struct Alka::Bind<DisposeTest::Mod> : Module<"disp"> {
    static constexpr auto members =
        Alka::members(Alka::type<DisposeTest::Meta>, Alka::type<DisposeTest::File>,
                      Alka::fn<&DisposeTest::keep>(), Alka::fn<&DisposeTest::keptName>(),
                      Alka::fn<&DisposeTest::global>());
};

namespace {

struct DisposeFixture : AlkaTest::V8Fixture {
    DisposeFixture() {
        DisposeTest::reset();
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<DisposeTest::Mod>(context);
        });
    }
    ~DisposeFixture() { DisposeTest::reset(); }
};

} // namespace

TEST(Dispose, ClosesAndDestroysAJSOwnedObjectRightAway) {
    DisposeFixture fx;
    fx.evalString("globalThis.f = new disp.File('a'); globalThis.meta = f.meta; f.dispose(); 'ok'");
    EXPECT_EQ(DisposeTest::File::sClosed, 1);
    EXPECT_EQ(DisposeTest::File::sDestroyed, 1);
    EXPECT_EQ(fx.evalString("try { f.name } catch (e) { e.message }"),
              "File.name: the native File object is not (or no longer) alive");
    EXPECT_EQ(fx.evalString("try { meta.size } catch (e) { e.message }"),
              "Meta.size: the native Meta object is not (or no longer) alive");
    fx.evalString("f.dispose(); 'ok'");
    EXPECT_EQ(DisposeTest::File::sClosed, 1);
    EXPECT_TRUE(
        fx.evalBool("f.dispose === f[Symbol.dispose] || typeof Symbol.dispose !== 'symbol'"));
}

TEST(Dispose, UsingDisposesAtTheEndOfTheBlock) {
    DisposeFixture fx;
    if (!fx.evalBool("typeof Symbol.dispose === 'symbol'")) {
        return;
    }
    fx.evalString("{ using f = new disp.File('b'); globalThis.wasOpen = f.open; } 'ok'");
    EXPECT_TRUE(fx.evalBool("wasOpen"));
    EXPECT_EQ(DisposeTest::File::sClosed, 1);
    EXPECT_EQ(DisposeTest::File::sDestroyed, 1);
}

TEST(Dispose, DisposingAnObjectCxxSharesEndsOnlyJSUseOfIt) {
    DisposeFixture fx;
    fx.evalString("globalThis.f = new disp.File('c'); disp.keep(f); f.dispose(); 'ok'");
    EXPECT_EQ(DisposeTest::File::sClosed, 1);
    EXPECT_EQ(DisposeTest::File::sDestroyed, 0);
    EXPECT_EQ(fx.evalString("disp.keptName()"), "c (closed)");
    EXPECT_TRUE(
        fx.evalBool("(() => { try { f.name; return false; } catch (e) { return true; } })()"));
    DisposeTest::gKept.reset();
    EXPECT_EQ(DisposeTest::File::sDestroyed, 1);
}

TEST(Dispose, ObjectDisposedDuringItsOwnNativeCallIsReleasedAfterTheCall) {
    DisposeFixture fx;
    fx.evalString("globalThis.f = new disp.File('d'); f.each(() => f.dispose()); 'ok'");
    EXPECT_EQ(DisposeTest::File::sClosed, 1);
    EXPECT_EQ(DisposeTest::File::sDestroyed, 1);
}

TEST(Dispose, BorrowedDisposableRunsItsHookAndOnlyDetachesFromJS) {
    DisposeFixture fx;
    fx.evalString("globalThis.g = disp.global(); g.dispose(); 'ok'");
    EXPECT_EQ(DisposeTest::File::sClosed, 1);
    EXPECT_EQ(DisposeTest::File::sDestroyed, 0);
    EXPECT_FALSE(DisposeTest::gGlobal.mOpen);
    EXPECT_TRUE(
        fx.evalBool("(() => { try { g.name; return false; } catch (e) { return true; } })()"));
}

TEST(Dispose, PendingCoroutineKeepsItsDisposedReceiverAliveUntilItSettles) {
    DisposeFixture fx;
    fx.evalString("globalThis.out = ''; globalThis.f = new disp.File('e');"
                  "f.slowName().then(n => out = n); f.dispose(); 'ok'");
    EXPECT_EQ(DisposeTest::File::sClosed, 1);
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("out") == "e"; }));
}

TEST(Dispose, DisposableClassesInTheDts) {
    const auto dts = Alka::generateDts<DisposeTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("        dispose(): void;"));
    EXPECT_THAT(dts, testing::HasSubstr("        [Symbol.dispose](): void;"));
    Alka::TsGenOptions options;
    options.mDisposableSymbol = false;
    EXPECT_THAT(Alka::generateDts<DisposeTest::Mod>(options),
                testing::Not(testing::HasSubstr("Symbol.dispose")));
}
