#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace HandleTest {

inline std::optional<Alka::Function<int()>> gCallback;
inline void store(Alka::Function<int()> fn) {
    gCallback = std::move(fn);
}

struct Mod;

} // namespace HandleTest

template <> struct Alka::Bind<HandleTest::Mod> : Module<"handles"> {
    static constexpr auto members = Alka::members(Alka::fn<&HandleTest::store>());
};

TEST(Handles, RefuseToRunOffTheirIsolateThread) {
    AlkaTest::V8Fixture fx;
    std::optional<Alka::Value> value;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        value.emplace(cx, v8::Number::New(isolate, 4));
    });
    std::string error;
    std::thread([&] {
        const auto result = value->to<int>();
        error = result ? std::string("ran") : result.error().message();
    }).join();
    EXPECT_EQ(error, "Alka handle used off its isolate's thread");
    EXPECT_EQ(value->to<int>().value(), 4);
}

TEST(Handles, HandleReleasedOnAnotherThreadIsReclaimedOnTheIsolateThread) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<HandleTest::Mod>(context);
    });
    fx.evalString("handles.store(() => 7); 'ok'");
    ASSERT_TRUE(HandleTest::gCallback.has_value());
    EXPECT_EQ(HandleTest::gCallback->call(), 7);

    auto moved = std::move(*HandleTest::gCallback);
    HandleTest::gCallback.reset();
    std::thread([fn = std::move(moved)]() mutable { fn = {}; }).join();
    fx.evalString("handles.store(() => 8); 'ok'");
    EXPECT_EQ(HandleTest::gCallback->call(), 8);
    HandleTest::gCallback.reset();
}

TEST(Handles, ReportADisposedIsolateInsteadOfDangling) {
    std::optional<Alka::Function<int()>> survivor;
    std::optional<Alka::Value> value;
    {
        AlkaTest::V8Fixture fx;
        fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
            Alka::ConvertCtx cx{isolate, context};
            const auto fn =
                AlkaTest::V8Fixture::compileAndRun(isolate, context, "() => 1").ToLocalChecked();
            survivor = Alka::fromJs<Alka::Function<int()>>(cx, fn).value();
            value.emplace(cx, v8::Number::New(isolate, 2));
        });
        EXPECT_EQ(survivor->call(), 1);
    }
    EXPECT_FALSE(survivor->valid());
    const auto called = survivor->tryCall();
    ASSERT_FALSE(called.has_value());
    EXPECT_EQ(called.error().message(), "the handle's isolate was disposed");
    EXPECT_EQ(value->to<int>().error().message(), "the handle's isolate was disposed");
    survivor.reset();
    value.reset();
}

TEST(Handles, WeakValueObservesWithoutKeepingAlive) {
    AlkaTest::V8Fixture fx;
    std::optional<Alka::WeakValue<Alka::Object>> weak;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        const v8::HandleScope scope(isolate);
        Alka::ConvertCtx cx{isolate, context};
        const auto object =
            AlkaTest::V8Fixture::compileAndRun(isolate, context, "globalThis.o = {x: 5}; o")
                .ToLocalChecked();
        weak.emplace(Alka::Object(cx, object.As<v8::Object>()));
    });
    EXPECT_FALSE(weak->expired());
    EXPECT_EQ(weak->lock().getAs<int>("x").value(), 5);
    fx.evalString("globalThis.o = null; 'ok'");
    fx.collectGarbage();
    EXPECT_TRUE(weak->expired());
    EXPECT_FALSE(weak->lock().valid());
}
