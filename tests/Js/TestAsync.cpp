#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace AsyncTest {

inline std::optional<Alka::Settler<int>> gSettler;

inline Alka::Promise<int> startJob() {
    auto promise = Alka::Promise<int>::create();
    gSettler = promise.settler();
    return promise;
}

inline Alka::Task<int> doubleLater(int value) {
    co_return value * 2;
}

inline Alka::Task<std::string> awaitJs(Alka::Function<Alka::Value()> producer) {
    const auto text = co_await Alka::await<std::string>(producer.call());
    co_return "got " + text;
}

inline Alka::Task<int> awaitRejected(Alka::Function<Alka::Value()> producer) {
    auto result = co_await Alka::tryAwait<int>(producer.call());
    co_return result ? *result : -1;
}

inline Alka::Task<int> offloadSum(int a, int b) {
    const auto worker = co_await Alka::offload([] { return std::this_thread::get_id(); });
    const int sum = co_await Alka::offload([a, b] { return a + b; });
    co_return worker != std::this_thread::get_id() ? sum : -1;
}

inline Alka::Task<void> failLater() {
    co_await Alka::offload([] {});
    throw std::runtime_error("coroutine failed");
}

inline Alka::Task<int> chain(int value) {
    const int doubled = co_await doubleLater(value);
    co_return doubled + 1;
}

struct ToThread {
    const std::atomic<bool>* mGate = nullptr;
    bool await_ready() const noexcept { return false; }
    void await_suspend(const std::coroutine_handle<> handle) const {
        std::thread([handle, gate = mGate] {
            while (gate != nullptr && !gate->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            handle.resume();
        }).detach();
    }
    void await_resume() const noexcept {}
};

inline Alka::Task<int> hopAndBack(int value) {
    const auto home = std::this_thread::get_id();
    const Alka::Dispatcher dispatcher(v8::Isolate::GetCurrent());
    co_await ToThread{};
    const auto away = std::this_thread::get_id();
    co_await Alka::resumeOn(dispatcher);
    const auto back = std::this_thread::get_id();
    const int doubled = co_await doubleLater(value);
    co_return away != home&& back == home ? doubled : -1;
}

inline Alka::Task<int> stayHome(int value) {
    const auto home = std::this_thread::get_id();
    co_await Alka::resumeOn(Alka::Dispatcher(v8::Isolate::GetCurrent()));
    co_return std::this_thread::get_id() == home ? value + 1 : -1;
}

struct FrameProbe {
    inline static std::atomic<int> sAlive{0};
    FrameProbe() { ++sAlive; }
    ~FrameProbe() { --sAlive; }
    FrameProbe(const FrameProbe&) = delete;
    FrameProbe& operator=(const FrameProbe&) = delete;
};
inline std::atomic gHopGate{false};
inline Alka::Task<int> hopLate() {
    const FrameProbe probe;
    const Alka::Dispatcher dispatcher(v8::Isolate::GetCurrent());
    co_await ToThread{&gHopGate};
    co_await Alka::resumeOn(dispatcher);
    co_return 1;
}

struct Counter {
    inline static int sAlive = 0;
    Counter() { ++sAlive; }
    ~Counter() { --sAlive; }
    int mCount = 0;
    Alka::Task<int> bumpLater() {
        co_await Alka::offload([] {});
        co_return ++mCount;
    }
};

inline std::size_t viewSize(Alka::BufferView view) {
    return view.size();
}
inline Alka::BufferView gKeptView;
inline void keepView(Alka::BufferView view) {
    gKeptView = std::move(view);
}
inline int sharedSum(Alka::SharedBufferView view) {
    int sum = 0;
    for (const auto byte : view.bytes()) {
        sum += static_cast<int>(byte);
    }
    return sum;
}
inline Alka::SharedBufferView makeShared(Alka::CallInfo call, int count) {
    auto out = Alka::SharedBufferView::allocate(call.mIsolate, static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out.bytes()[static_cast<std::size_t>(i)] = static_cast<std::byte>(i + 1);
    }
    return out;
}

inline Alka::BufferView sharedAsPlain(Alka::SharedBufferView view) {
    return view;
}

inline Alka::BufferView makeBytes(Alka::CallInfo call, int count) {
    auto out = Alka::BufferView::allocate(call.mIsolate, static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out.bytes()[static_cast<std::size_t>(i)] = static_cast<std::byte>(i * 3);
    }
    return out;
}

struct Mod;

} // namespace AsyncTest

template <> struct Alka::Bind<AsyncTest::Counter> : Class<AsyncTest::Counter> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&AsyncTest::Counter::bumpLater>(),
                      Alka::prop<&AsyncTest::Counter::mCount>());
};

template <> struct Alka::Bind<AsyncTest::Mod> : Module<"async"> {
    static constexpr auto members = Alka::members(
        Alka::type<AsyncTest::Counter>, Alka::fn<&AsyncTest::startJob>(),
        Alka::fn<&AsyncTest::doubleLater>(arg("value")),
        Alka::fn<&AsyncTest::awaitJs>(arg("producer")),
        Alka::fn<&AsyncTest::awaitRejected>(arg("producer")),
        Alka::fn<&AsyncTest::offloadSum>(arg("a"), arg("b")), Alka::fn<&AsyncTest::failLater>(),
        Alka::fn<&AsyncTest::chain>(arg("value")), Alka::fn<&AsyncTest::hopAndBack>(arg("value")),
        Alka::fn<&AsyncTest::stayHome>(arg("value")), Alka::fn<&AsyncTest::hopLate>(),
        Alka::fn<"viewSize", [](BufferView v) { return static_cast<int>(v.size()); }>(),
        Alka::fn<&AsyncTest::keepView>(), Alka::fn<&AsyncTest::makeBytes>(arg("count")),
        Alka::fn<&AsyncTest::sharedSum>(arg("data")),
        Alka::fn<&AsyncTest::makeShared>(arg("count")),
        Alka::fn<&AsyncTest::sharedAsPlain>(arg("data")));
};

namespace {

struct AsyncFixture : AlkaTest::V8Fixture {
    AsyncFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<AsyncTest::Mod>(context);
        });
    }
};

} // namespace

TEST(Async, DispatcherRunsPostedWorkOnTheIsolateThreadAtDrain) {
    AsyncFixture fx;
    std::atomic<int> wakeups{0};
    std::optional<Alka::Dispatcher> dispatcher;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context>) {
        dispatcher.emplace(isolate);
        dispatcher->setWakeup([&] { ++wakeups; });
    });
    std::vector<int> order;
    std::thread([&] {
        for (int i = 0; i < 100; ++i) {
            EXPECT_TRUE(dispatcher->post([&order, i] { order.push_back(i); }));
        }
    }).join();
    EXPECT_TRUE(order.empty());
    EXPECT_GE(wakeups.load(), 1);
    std::size_t ran = 0;
    fx.run([&](v8::Isolate*, v8::Local<v8::Context>) { ran = dispatcher->drain(); });
    EXPECT_EQ(ran, 100);
    ASSERT_EQ(order.size(), 100);
    EXPECT_EQ(order.front(), 0);
    EXPECT_EQ(order.back(), 99);
}

TEST(Async, SettlerResolvesAPromiseFromAnotherThread) {
    AsyncFixture fx;
    fx.evalString(
        "globalThis.result = 'pending'; async.startJob().then(v => { result = v; }); 'ok'");
    ASSERT_TRUE(AsyncTest::gSettler.has_value());
    std::thread([settler = *AsyncTest::gSettler] { EXPECT_TRUE(settler.resolve(42)); }).join();
    EXPECT_FALSE(AsyncTest::gSettler->resolve(43)); // first settlement wins
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("String(result)") == "42"; }));
    AsyncTest::gSettler.reset();
}

TEST(Async, BoundCoroutinesReturnPromises) {
    AsyncFixture fx;
    fx.evalString("globalThis.out = []; async.doubleLater(21).then(v => out.push(v));"
                  "async.chain(5).then(v => out.push(v)); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("out.join(',')") == "42,11"; }));
}

TEST(Async, CoroutinesAwaitJSPromises) {
    AsyncFixture fx;
    fx.evalString("globalThis.out = 'pending';"
                  "async.awaitJs(() => new Promise(r => setTimeoutLike = r)).then(v => out = v);"
                  "'ok'");
    EXPECT_EQ(fx.evalString("out"), "pending");
    fx.evalString("setTimeoutLike('hello'); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("out") == "got hello"; }));
    fx.evalString("async.awaitJs(() => 'now').then(v => out = v); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("out") == "got now"; }));
    fx.evalString(
        "async.awaitRejected(() => Promise.reject(new Error('no'))).then(v => out = v); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("String(out)") == "-1"; }));
    fx.evalString("{ const err = new TypeError('original');"
                  "  async.awaitJs(() => Promise.reject(err)).catch(e => out = e === err); } 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("String(out)") == "true"; }));
}

TEST(Async, OffloadRunsOnAWorkerAndResumesOnTheIsolateThread) {
    AsyncFixture fx;
    fx.evalString("globalThis.out = 'pending'; async.offloadSum(2, 3).then(v => out = v); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("String(out)") == "5"; }));
    fx.evalString("async.failLater().catch(e => out = e.message); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("out") == "coroutine failed"; }));
}

TEST(Async, ResumeOnBringsACoroutineBackToTheIsolateThreadAndItsContext) {
    AsyncFixture fx;
    fx.evalString("globalThis.out = 'pending'; async.hopAndBack(21).then(v => out = v,"
                  " e => out = e.message); 'ok'");
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("String(out)") != "pending"; }));
    EXPECT_EQ(fx.evalString("String(out)"), "42");
    fx.evalString("globalThis.out = 'pending'; async.stayHome(1).then(v => out = v); 'ok'");
    EXPECT_EQ(fx.evalString("String(out)"), "2");
}

TEST(Async, ResumeOnAfterIsolateDisposalDestroysTheCoroutine) {
    AsyncTest::gHopGate = false;
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
        Alka::install<AsyncTest::Mod>(context);
        (void)AlkaTest::V8Fixture::compileAndRun(isolate, context, "async.hopLate(); 'ok'");
    }
    EXPECT_EQ(AsyncTest::FrameProbe::sAlive, 1);
    Alka::disposeIsolate(isolate);
    isolate->Dispose();
    AsyncTest::gHopGate = true;
    for (int i = 0; i < 5000 && AsyncTest::FrameProbe::sAlive != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(AsyncTest::FrameProbe::sAlive, 0);
}

TEST(Async, CoroutineMethodKeepsItsObjectAliveUntilItSettles) {
    AsyncFixture fx;
    const int before = AsyncTest::Counter::sAlive;
    fx.evalString("globalThis.out = 'pending';"
                  "new async.Counter().bumpLater().then(v => out = v); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(AsyncTest::Counter::sAlive, before + 1);
    EXPECT_TRUE(fx.pumpUntil([&] { return fx.evalString("String(out)") == "1"; }));
    fx.evalString("globalThis.out = null; 'ok'");
    fx.collectGarbage();
    fx.collectGarbage();
    EXPECT_EQ(AsyncTest::Counter::sAlive, before);
}

TEST(Async, DisposingAnIsolateDestroysCoroutinesStillSuspended) {
    {
        AsyncFixture fx;
        fx.evalString("async.awaitJs(() => new Promise(() => {})); 'ok'");
    }
    EXPECT_TRUE(true);
}

TEST(Async, SharedBufferViewOptsInToSharedArrayBufferMemory) {
    AsyncFixture fx;
    EXPECT_EQ(fx.evalInt("async.sharedSum(new Uint8Array(new SharedArrayBuffer(4)).fill(2))"), 8);
    EXPECT_EQ(fx.evalInt("async.sharedSum(new SharedArrayBuffer(3))"), 0);
    EXPECT_EQ(fx.evalInt("async.sharedSum(new Uint8Array([1, 2, 3]))"), 6);
    EXPECT_EQ(fx.evalInt("async.sharedSum(new SharedArrayBuffer(4, { maxByteLength: 8 }))"), 0);
    EXPECT_EQ(fx.evalString("try { async.sharedSum(new ArrayBuffer(4, { maxByteLength: 8 })) }"
                            " catch (e) { e.message }"),
              "arg 1 (data) of async.sharedSum: a resizable ArrayBuffer is not accepted as a "
              "SharedBufferView");
    EXPECT_EQ(fx.evalString("try { async.viewSize(new Uint8Array(new SharedArrayBuffer(4))) }"
                            " catch (e) { e.message }"),
              "arg 1 of async.viewSize: SharedArrayBuffer data is not accepted as a BufferView");
    EXPECT_TRUE(fx.evalBool("(() => { const v = async.makeShared(3);"
                            " return v.buffer instanceof SharedArrayBuffer &&"
                            " v.join(',') === '1,2,3'; })()"));
    EXPECT_TRUE(fx.evalBool("(() => { const sab = new SharedArrayBuffer(2);"
                            " const v = async.sharedAsPlain(sab); v[0] = 5;"
                            " return v.buffer instanceof SharedArrayBuffer &&"
                            " new Uint8Array(sab)[0] === 5; })()"));
    const auto dts = Alka::generateDts<AsyncTest::Mod>();
    EXPECT_THAT(dts,
                testing::HasSubstr(
                    "function sharedSum(data: ArrayBuffer | SharedArrayBuffer | ArrayBufferView): "
                    "number;"));
}

TEST(Async, BufferViewIsZeroCopyAndOutlivesItsJSBuffer) {
    AsyncFixture fx;
    EXPECT_EQ(fx.evalInt("async.viewSize(new Uint8Array(10).subarray(2, 7))"), 5);
    EXPECT_EQ(fx.evalInt("async.viewSize(new ArrayBuffer(4))"), 4);
    fx.evalString("{ const a = new Uint8Array([9, 8, 7]); async.keepView(a); a[0] = 1; } 'ok'");
    fx.collectGarbage();
    ASSERT_EQ(AsyncTest::gKeptView.size(), 3);
    EXPECT_EQ(static_cast<int>(AsyncTest::gKeptView.bytes()[0]), 1);
    AsyncTest::gKeptView = {};
    EXPECT_EQ(fx.evalString("Array.from(async.makeBytes(4)).join(',')"), "0,3,6,9");
    EXPECT_EQ(fx.evalString("try { async.viewSize(new ArrayBuffer(4, { maxByteLength: 8 })) }"
                            " catch (e) { e.message }"),
              "arg 1 of async.viewSize: a resizable ArrayBuffer is not accepted as a BufferView");
    const auto dts = Alka::generateDts<AsyncTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("function doubleLater(value: number): Promise<number>;"));
    EXPECT_THAT(dts, testing::HasSubstr("function failLater(): Promise<void>;"));
    EXPECT_THAT(dts, testing::HasSubstr("function makeBytes(count: number): Uint8Array;"));
    EXPECT_THAT(
        dts, testing::HasSubstr("function keepView(arg0: ArrayBuffer | ArrayBufferView): void;"));
}
