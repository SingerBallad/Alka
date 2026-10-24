#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>
#include <Alka/Runtime/Snapshot.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SnapTest {

struct Counter {
    int mCount = 0;
    int bump() { return ++mCount; }
};

inline int add(int a, int b) {
    return a + b;
}

struct Pair {
    int mA = 0;
    int mB = 0;
};
inline Pair swapped(Pair p) {
    return Pair{p.mB, p.mA};
}
inline Counter gCounter;
inline Counter* shared() {
    return &gCounter;
}

struct Mod;

} // namespace SnapTest

template <> struct Alka::Bind<SnapTest::Counter> : Class<SnapTest::Counter> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto lifetime = Lifetime::Static;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&SnapTest::Counter::bump>(),
                      Alka::prop<&SnapTest::Counter::mCount>());
};

template <> struct Alka::Bind<SnapTest::Pair> : Record<SnapTest::Pair> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::prop<&SnapTest::Pair::mA>(), Alka::prop<&SnapTest::Pair::mB>());
};

template <> struct Alka::Bind<SnapTest::Mod> : Module<"snap"> {
    static constexpr auto members = Alka::members(
        Alka::type<SnapTest::Counter>, Alka::type<SnapTest::Pair>, Alka::fn<&SnapTest::add>(),
        Alka::fn<&SnapTest::shared>(), Alka::fn<&SnapTest::swapped>());
};

namespace {

std::string eval(v8::Isolate* isolate, v8::Local<v8::Context> context, std::string_view source) {
    const v8::TryCatch tryCatch(isolate);
    v8::Local<v8::Value> value;
    if (!AlkaTest::V8Fixture::compileAndRun(isolate, context, source).ToLocal(&value)) {
        const v8::String::Utf8Value text(isolate, tryCatch.Exception());
        return std::string("threw: ") + (*text != nullptr ? *text : "?");
    }
    const v8::String::Utf8Value text(isolate, value);
    return *text != nullptr ? *text : "";
}

} // namespace

TEST(Snapshot, BindingsSurviveAStartupSnapshot) {
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator.get();
    params.external_references = Alka::snapshot::externalReferences<SnapTest::Mod>();

    v8::StartupData blob{nullptr, 0};
    std::vector<std::size_t> templates;
    {
        v8::SnapshotCreator creator(params);
        v8::Isolate* isolate = creator.GetIsolate();
        {
            const v8::Isolate::Scope isolateScope(isolate);
            const v8::HandleScope handleScope(isolate);
            const auto context = v8::Context::New(isolate);
            const v8::Context::Scope contextScope(context);
            Alka::install<SnapTest::Mod>(context);
            EXPECT_EQ(
                eval(isolate, context,
                     "globalThis.made = new snap.Counter(); made.bump();"
                     "globalThis.ready = snap.add(2, 3); globalThis.Cls = snap.Counter; 'ok'"),
                "ok");
            EXPECT_EQ(eval(isolate, context, "snap.swapped({ a: 1, b: 2 }).a"), "2");
            templates = Alka::snapshot::addTemplates<SnapTest::Mod>(creator);
            creator.SetDefaultContext(context, Alka::snapshot::internalFieldsSerializer());
        }
        Alka::snapshot::prepare(isolate);
        blob = creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
    }
    ASSERT_GT(blob.raw_size, 0);

    params.snapshot_blob = &blob;
    v8::Isolate* isolate = v8::Isolate::New(params);
    Alka::snapshot::restoreTemplates<SnapTest::Mod>(isolate, templates);
    {
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope handleScope(isolate);
        const auto context = v8::Context::New(isolate, nullptr, {}, {},
                                              Alka::snapshot::internalFieldsDeserializer());
        const v8::Context::Scope contextScope(context);
        EXPECT_EQ(eval(isolate, context, "ready"), "5");
        EXPECT_EQ(eval(isolate, context, "snap.add(4, 5)"), "9");
        EXPECT_EQ(eval(isolate, context, "snap.swapped({ a: 7, b: 8 }).b"), "7");
        EXPECT_EQ(eval(isolate, context, "new snap.Counter().bump()"), "1");
        EXPECT_EQ(eval(isolate, context, "Cls === snap.Counter"), "true");
        EXPECT_EQ(eval(isolate, context, "snap.shared() instanceof snap.Counter"), "true");
        EXPECT_EQ(
            eval(isolate, context, "try { made.bump() } catch (e) { e.message }"),
            "Counter.bump: the native Counter object is missing (never constructed -- missing "
            "super()? -- or restored from a snapshot)");
    }
    Alka::disposeIsolate(isolate);
    isolate->Dispose();
    delete[] blob.data;
}
