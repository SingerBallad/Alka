#include <Alka/Alka.hpp>
#include <Alka/Engine/Engine.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EngineTest {

inline int gGauges = 0;

struct Gauge {
    Gauge() { ++gGauges; }
    Gauge(const Gauge&) = delete;
    Gauge& operator=(const Gauge&) = delete;
    ~Gauge() { --gGauges; }
};

struct Mod;

} // namespace EngineTest

template <> struct Alka::Bind<EngineTest::Gauge> : Class<EngineTest::Gauge> {
    static constexpr auto members = Alka::members(Alka::ctor<>());
};

template <> struct Alka::Bind<EngineTest::Mod> : Module<"engineTest"> {
    static constexpr auto members = Alka::members(Alka::type<EngineTest::Gauge>);
};

namespace {

struct ConsoleCapture {
    std::vector<std::pair<Alka::ConsoleLevel, std::string>> mLines;

    [[nodiscard]] auto options() -> Alka::ContextOptions {
        Alka::ContextOptions options;
        options.mConsoleSink = [this](const Alka::ConsoleLevel level, const std::string_view text) {
            mLines.emplace_back(level, std::string(text));
        };
        return options;
    }
    [[nodiscard]] auto text(const std::size_t i) const -> const std::string& {
        return mLines.at(i).second;
    }
};

void writeFile(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

auto run(const Alka::ContextScope& scope, const std::string_view source) -> std::string {
    const auto result = scope.evalToString(source);
    if (!result) {
        ADD_FAILURE() << "JS failed: " << result.error().message();
        return {};
    }
    return *result;
}

} // namespace

TEST(Engine, ConsoleFormatsLikeNodeAndReachesTheSink) {
    auto isolate = Alka::Engine::createIsolate();
    ConsoleCapture capture;
    const Alka::ContextScope scope(isolate.get(), capture.options());
    ASSERT_TRUE(scope
                    .eval(R"(
        console.log('hello', 42, 'x');
        console.log({ a: 1, b: 'two', c: [1, 2, { d: null }], 'e-f': true });
        console.log('%s is %d, %i, %j', 'Ann', 42.5, 42.5, { k: [1] });
        console.warn(new Map([[1, 'one']]), new Set(['s']));
        console.error([1, , 3]);
        const o = { name: 'self' }; o.self = o; console.info(o);
        console.debug(undefined, null, -0, 10n, Symbol('s'), function foo() {}, class Bar {});
        console.group('g'); console.log('inner\nlines'); console.groupEnd(); console.log('outer');
        console.count(); console.count(); console.count('x');
        console.log(new Uint8Array([1, 2]), [], {}, Object.create(null));
        console.log('%o and %%', 'str', 'tail');
        console.log({ deep: { deeper: { deepest: { gone: 1 } } } });
        console.assert(1 === 2, 'math');
        console.assert(true, 'never');
    )")
                    .has_value());
    using enum Alka::ConsoleLevel;
    const std::vector<std::pair<Alka::ConsoleLevel, std::string>> expected = {
        {Log, "hello 42 x"},
        {Log, "{ a: 1, b: 'two', c: [ 1, 2, { d: null } ], 'e-f': true }"},
        {Log, "Ann is 42.5, 42, {\"k\":[1]}"},
        {Warn, "Map(1) { 1 => 'one' } Set(1) { 's' }"},
        {Error, "[ 1, <empty>, 3 ]"},
        {Info, "{ name: 'self', self: [Circular] }"},
        {Debug, "undefined null -0 10n Symbol(s) [Function: foo] [class Bar]"},
        {Log, "g"},
        {Log, "  inner\n  lines"},
        {Log, "outer"},
        {Log, "default: 1"},
        {Log, "default: 2"},
        {Log, "x: 1"},
        {Log, "Uint8Array(2) [ 1, 2 ] [] {} [Object: null prototype] {}"},
        {Log, "'str' and % tail"},
        {Log, "{ deep: { deeper: { deepest: [Object] } } }"},
        {Error, "Assertion failed: math"},
    };
    ASSERT_EQ(capture.mLines.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE("i = " + std::to_string(i));
        EXPECT_EQ(capture.mLines[i].first, expected[i].first);
        EXPECT_EQ(capture.mLines[i].second, expected[i].second);
    }
}

TEST(Engine, TimersRunInDueOrderUntilIdle) {
    auto isolate = Alka::Engine::createIsolate();
    const Alka::ContextScope scope(isolate.get());
    ASSERT_TRUE(scope
                    .eval(R"(
        globalThis.order = [];
        setTimeout(() => order.push('late'), 200);
        setTimeout((a, b) => order.push('t0:' + a + b), 0, 'x', 'y');
        setImmediate(() => order.push('immediate'));
        const cancelled = setTimeout(() => order.push('never'), 5);
        clearTimeout(cancelled);
        let n = 0;
        // (due by time, as in Node: 20 ms puts it after the 0 ms timers even on a
        // loaded machine, where registering the last one may take over 1 ms)
        const iv = setInterval(() => { order.push('iv' + (++n)); if (n === 3) clearInterval(iv); }, 20);
        queueMicrotask(() => order.push('micro'));
        setTimeout(() => Promise.resolve().then(() => order.push('then')), 0);
    )")
                    .has_value());
    EXPECT_TRUE(Alka::Engine::runUntilIdle(isolate.get()));
    EXPECT_EQ(run(scope, "order.join(',')"), "micro,t0:xy,immediate,then,iv1,iv2,iv3,late");
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(Alka::Engine::runUntilIdle(isolate.get()));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST(Engine, TimerWhoseDelayThrowsIsNeverScheduled) {
    auto isolate = Alka::Engine::createIsolate();
    const Alka::ContextScope scope(isolate.get());
    EXPECT_FALSE(scope
                     .eval("globalThis.ran = false; setTimeout(() => { ran = true; },"
                           " { valueOf() { throw new Error('bad delay'); } })")
                     .has_value());
    EXPECT_TRUE(Alka::Engine::runUntilIdle(isolate.get()));
    EXPECT_EQ(run(scope, "String(ran)"), "false");
}

TEST(Engine, IsolateGoingDestroysTheCxxObjectsJSStillOwned) {
    {
        auto isolate = Alka::Engine::createIsolate();
        const Alka::ContextScope scope(isolate.get());
        scope.install<EngineTest::Mod>();
        ASSERT_TRUE(scope.eval("globalThis.kept = [new engineTest.Gauge(), new engineTest.Gauge()]")
                        .has_value());
        EXPECT_EQ(EngineTest::gGauges, 2);
    }
    EXPECT_EQ(EngineTest::gGauges, 0);
}

TEST(Engine, UnhandledErrorsAreReportedAndFailTheRun) {
    std::vector<std::string> reports;
    auto isolate = Alka::Engine::createIsolate(
        {.mOnError = [&](const std::string_view message) { reports.emplace_back(message); }});
    const Alka::ContextScope scope(isolate.get());

    ASSERT_TRUE(scope.eval("Promise.reject(new Error('lost')); Promise.reject(1).catch(() => {});")
                    .has_value());
    EXPECT_FALSE(Alka::Engine::runUntilIdle(isolate.get()));
    ASSERT_EQ(reports.size(), 1);
    EXPECT_THAT(reports[0], testing::StartsWith("Uncaught (in promise) Error: lost"));

    reports.clear();
    ASSERT_TRUE(scope
                    .eval("setTimeout(() => { throw new TypeError('boom'); }, 0); "
                          "setTimeout(() => { globalThis.after = true; }, 1);")
                    .has_value());
    EXPECT_FALSE(Alka::Engine::runUntilIdle(isolate.get()));
    ASSERT_EQ(reports.size(), 1);
    EXPECT_THAT(reports[0], testing::StartsWith("Uncaught TypeError: boom"));
    EXPECT_EQ(run(scope, "globalThis.after"), "true");

    reports.clear();
    ASSERT_TRUE(scope
                    .eval("queueMicrotask(() => { throw new RangeError('micro'); }); "
                          "reportError(new Error('reported'));")
                    .has_value());
    EXPECT_FALSE(Alka::Engine::runUntilIdle(isolate.get()));
    ASSERT_EQ(reports.size(), 2);
    EXPECT_THAT(reports[0], testing::StartsWith("Uncaught Error: reported"));
    EXPECT_THAT(reports[1], testing::StartsWith("Uncaught RangeError: micro"));

    // a clean run succeeds again
    reports.clear();
    ASSERT_TRUE(scope.eval("setTimeout(() => {}, 0)").has_value());
    EXPECT_TRUE(Alka::Engine::runUntilIdle(isolate.get()));
    EXPECT_TRUE(reports.empty());
}

TEST(Engine, ScriptPastItsTimeLimitIsTerminated) {
    auto isolate = Alka::Engine::createIsolate();
    const Alka::ContextScope scope(isolate.get());
    const auto spun =
        scope.eval("for (;;) {}", "<spin>", {.mTimeout = std::chrono::milliseconds(50)});
    ASSERT_FALSE(spun.has_value());
    EXPECT_EQ(spun.error().message(), "the script timed out after 50 ms");
    EXPECT_EQ(run(scope, "1 + 1"), "2"); // usable again
    const auto quick =
        scope.evalToString("'ok'", "<quick>", {.mTimeout = std::chrono::seconds(30)});
    ASSERT_TRUE(quick.has_value());
    EXPECT_EQ(*quick, "ok");

    const auto module = scope.evalModule("while (true) {}", "<spin-module>",
                                         {.mTimeout = std::chrono::milliseconds(50)});
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().message(), "the script timed out after 50 ms");
    EXPECT_EQ(run(scope, "2 + 2"), "4");
}

TEST(Engine, HeapLimitStopsAScriptNotTheProcess) {
    auto isolate = Alka::Engine::createIsolate({.mMaxHeapBytes = std::size_t{64} << 20});
    const Alka::ContextScope scope(isolate.get());
    const auto hog =
        scope.eval("(() => { const hog = []; for (;;) hog.push(new Array(100000).fill(1.5)); })()");
    ASSERT_FALSE(hog.has_value());
    EXPECT_EQ(hog.error().message(), "JS heap limit reached (64 MB)");
    EXPECT_EQ(run(scope, "[1, 2, 3].map((x) => x * 2).join()"), "2,4,6"); // usable again
}

TEST(Engine, WebGlobals) {
    auto isolate = Alka::Engine::createIsolate();
    const Alka::ContextScope scope(isolate.get());

    EXPECT_EQ(run(scope, R"(
        const c = new AbortController();
        const seen = [];
        c.signal.addEventListener('abort', (e) => seen.push(e.type + ':' + (e.target === c.signal)));
        c.signal.onabort = () => seen.push('onabort');
        c.signal.addEventListener('abort', { handleEvent: () => seen.push('object') }, { once: true });
        c.abort();
        c.abort('again');  // only once
        [c.signal.aborted, c.signal.reason.name, seen.join('+')].join()
    )"),
              "true,AbortError,abort:true+onabort+object");
    EXPECT_EQ(run(scope, R"(
        const reason = new Error('mine');
        const s = AbortSignal.abort(reason);
        let thrown;
        try { s.throwIfAborted(); } catch (e) { thrown = e; }
        [thrown === reason, AbortSignal.any([new AbortController().signal, s]).reason === reason].join()
    )"),
              "true,true");
    EXPECT_EQ(run(scope, "try { new AbortSignal(); 'made' } catch (e) { e.constructor.name }"),
              "TypeError");
    ASSERT_TRUE(
        scope
            .eval(
                "globalThis.t = AbortSignal.timeout(5); globalThis.keep = setTimeout(() => {}, 30)")
            .has_value());
    EXPECT_TRUE(Alka::Engine::runUntilIdle(isolate.get()));
    EXPECT_EQ(run(scope, "t.aborted + ',' + t.reason.name"), "true,TimeoutError");
    ASSERT_TRUE(scope.eval("globalThis.t2 = AbortSignal.timeout(60000)").has_value());
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(Alka::Engine::runUntilIdle(isolate.get()));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
    EXPECT_EQ(run(scope, "t2.aborted"), "false");

    EXPECT_EQ(run(scope, R"(
        class Emitter extends EventTarget {}
        const e = new Emitter();
        const got = [];
        const ac = new AbortController();
        e.addEventListener('ping', (ev) => { got.push('a'); ev.stopImmediatePropagation(); });
        e.addEventListener('ping', () => got.push('b'));
        e.addEventListener('pong', () => got.push('pong'), { signal: ac.signal });
        const ok = e.dispatchEvent(new Event('ping', { cancelable: true }));
        ac.abort();
        e.dispatchEvent(new Event('pong'));
        got.join() + '|' + ok
    )"),
              "a|true");

    EXPECT_EQ(
        run(scope, "Array.from(new TextEncoder().encode('h\\u00e9\\u4f60\\ud83d\\ude00')).join()"),
        "104,195,169,228,189,160,240,159,152,128");
    EXPECT_EQ(
        run(scope, "new TextDecoder().decode(new Uint8Array([0xEF, 0xBB, 0xBF, 104, 195, 169]))"),
        "h\xC3\xA9");
    EXPECT_EQ(run(scope, R"(
        const d = new TextDecoder();
        const bytes = new TextEncoder().encode('你好');
        d.decode(bytes.subarray(0, 2), { stream: true }) + d.decode(bytes.subarray(2))
    )"),
              "\xE4\xBD\xA0\xE5\xA5\xBD");
    EXPECT_EQ(run(scope, "new TextDecoder().decode(new Uint8Array([0xFF, 65]))"), "\xEF\xBF\xBD"
                                                                                  "A");
    EXPECT_EQ(run(scope, R"(
        try { new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xC0, 0x80])); 'decoded' }
        catch (e) { e.constructor.name }
    )"),
              "TypeError");
    EXPECT_EQ(run(scope, R"(
        const out = new Uint8Array(4);
        const r = new TextEncoder().encodeInto('aé你', out);
        r.read + ',' + r.written + ',' + Array.from(out).join(' ')
    )"),
              "2,3,97 195 169 0");

    EXPECT_EQ(run(scope, R"(
        const src = { n: 1, list: [1, { x: 2 }], when: new Date(0), map: new Map([[1, 'a']]) };
        src.self = src;
        const copy = structuredClone(src);
        [copy !== src, copy.self === copy, copy.list[1].x, copy.when.getTime(), copy.map.get(1)].join()
    )"),
              "true,true,2,0,a");
    EXPECT_EQ(run(scope, R"(
        const buffer = new Uint8Array([1, 2, 3]).buffer;
        const moved = structuredClone(buffer, { transfer: [buffer] });
        buffer.byteLength + ',' + new Uint8Array(moved).join(' ')
    )"),
              "0,1 2 3");
    EXPECT_EQ(run(scope, "try { structuredClone(() => 1); 'cloned' } catch (e) { 'refused' }"),
              "refused");

    // performance
    EXPECT_EQ(run(scope, R"(
        const a = performance.now();
        const b = performance.now();
        [typeof a, b >= a, performance.timeOrigin > 1.6e12].join()
    )"),
              "number,true,true");
}

TEST(Engine, ContextsOptOutOfWhatTheyDoNotWant) {
    auto isolate = Alka::Engine::createIsolate();
    const Alka::ContextScope scope(isolate.get(), {.mTimers = false, .mWebGlobals = false});
    EXPECT_EQ(
        run(scope, "[typeof setTimeout, typeof AbortController, typeof structuredClone].join()"),
        "undefined,undefined,undefined");
}

TEST(Engine, ModuleFilesImportFilesJSONAndDynamically) {
    const auto dir = std::filesystem::temp_directory_path() / "alka engine modules";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "lib");
    writeFile(dir / "main.mjs", R"(
        import { twice } from './lib/math.mjs';
        import data from './data.json' with { type: 'json' };
        import index from './lib';
        export const result = twice(data.n);
        export const fromIndex = index;
        export const url = import.meta.url;
        export const lazy = (await import('./lib/math.mjs')).twice(100);
        export const same = (await import('./lib/math.js')).twice === twice;
    )");
    writeFile(dir / "lib" / "math.js", "export const twice = (x) => x * 2;");
    writeFile(dir / "lib" / "math.mjs", "export { twice } from './math.js';");
    writeFile(dir / "lib" / "index.mjs", "export default 'index';");
    writeFile(dir / "data.json", R"({"n": 21})");

    auto isolate = Alka::Engine::createIsolate();
    {
        const Alka::ContextScope scope(isolate.get());
        const auto ns = scope.runModuleFile(dir / "main.mjs");
        ASSERT_TRUE(ns.has_value()) << (ns ? std::string() : ns.error().message());
        const auto object = ns->to<Alka::Object>().value();
        EXPECT_EQ(object.getAs<int>("result").value(), 42);
        EXPECT_EQ(object.getAs<std::string>("fromIndex").value(), "index");
        EXPECT_EQ(object.getAs<int>("lazy").value(), 200);
        EXPECT_TRUE(object.getAs<bool>("same").value());
        const auto url = object.getAs<std::string>("url").value();
        EXPECT_THAT(url, testing::StartsWith("file:///"));
        EXPECT_THAT(url, testing::EndsWith("/alka%20engine%20modules/main.mjs"));

        const auto missing = scope.evalModule("import x from 'nowhere';");
        ASSERT_FALSE(missing.has_value());
        EXPECT_THAT(missing.error().message(), testing::HasSubstr("Cannot find module 'nowhere'"));
        const auto absent = scope.evalModule("import x from './absent.mjs';", "<absent>");
        ASSERT_FALSE(absent.has_value());
        EXPECT_THAT(absent.error().message(),
                    testing::HasSubstr("Cannot find module './absent.mjs'"));
    }
    {
        const Alka::ContextScope scope(isolate.get(), {.mModuleRoot = dir});
        ASSERT_TRUE(
            scope
                .eval("import('./lib/math.mjs').then((m) => { globalThis.got = m.twice(4); }); "
                      "import('./nope.mjs').catch((e) => { globalThis.failed = e.message; });")
                .has_value());
        EXPECT_TRUE(Alka::Engine::runUntilIdle(isolate.get()));
        EXPECT_EQ(run(scope, "got"), "8");
        EXPECT_THAT(run(scope, "failed"), testing::StartsWith("Cannot find module './nope.mjs'"));
    }
    std::filesystem::remove_all(dir);
}

TEST(Engine, HostOwnIsolateWorksTooAndIsCleanedUpOnDispose) {
    v8::Isolate::CreateParams params;
    const std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    params.array_buffer_allocator = allocator.get();
    auto* isolate = v8::Isolate::New(params);
    {
        ConsoleCapture capture;
        const Alka::ContextScope scope(isolate, capture.options());
        ASSERT_TRUE(
            scope.eval("setTimeout(() => console.log('tick'), 1); Promise.reject(new Error('x'))")
                .has_value());
        EXPECT_FALSE(Alka::Engine::runUntilIdle(isolate));
        ASSERT_EQ(capture.mLines.size(), 1);
        EXPECT_EQ(capture.text(0), "tick");
        ASSERT_TRUE(
            scope.eval("setTimeout(() => {}, 100000); Promise.reject(new Error('y'))").has_value());
    }
    Alka::disposeIsolate(isolate);
    isolate->Dispose();
}
