#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>
#include <Alka/Engine/Engine.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace EsmTest {

struct Vec {
    Vec(double x, double y) : mX(x), mY(y) {}
    double mX;
    double mY;
    [[nodiscard]] double dot(const Vec& other) const { return mX * other.mX + mY * other.mY; }
};

enum class Mode { Off, On };

inline int twice(int v) {
    return v * 2;
}

struct GeoMod;
struct GeoUtilMod;

} // namespace EsmTest

template <> struct Alka::Bind<EsmTest::Vec> : Class<EsmTest::Vec> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<double, double>(), Alka::method<&EsmTest::Vec::dot>(),
                      Alka::prop<&EsmTest::Vec::mX>(), Alka::prop<&EsmTest::Vec::mY>());
};

template <> struct Alka::Bind<EsmTest::GeoUtilMod> : Module<"geo.util"> {
    static constexpr auto members = Alka::members(Alka::fn<&EsmTest::twice>());
};

template <> struct Alka::Bind<EsmTest::GeoMod> : Module<"geo"> {
    static constexpr auto members =
        Alka::members(Alka::type<EsmTest::Vec>, Alka::enumType<EsmTest::Mode>(),
                      Alka::fn<"norm2", [](const EsmTest::Vec& v) { return v.dot(v); }>(),
                      Alka::var<"ORIGIN_NAME">("origin"), Alka::submodule<EsmTest::GeoUtilMod>);
};

namespace {

std::string runModule(AlkaTest::V8Fixture& fx, std::string_view source,
                      std::string_view exportName) {
    return fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) -> std::string {
        const v8::TryCatch tryCatch(isolate);
        const auto str = [&](const std::string_view text) {
            return v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal,
                                           static_cast<int>(text.size()))
                .ToLocalChecked();
        };
        v8::ScriptOrigin origin(str("test.mjs"), 0, 0, false, -1, v8::Local<v8::Value>(), false,
                                false, true);
        v8::ScriptCompiler::Source compileSource(str(source), origin);
        v8::Local<v8::Module> module;
        v8::Local<v8::Value> completion;
        if (!v8::ScriptCompiler::CompileModule(isolate, &compileSource).ToLocal(&module) ||
            !module->InstantiateModule(context, &Alka::moduleResolveCallback).FromMaybe(false) ||
            !module->Evaluate(context).ToLocal(&completion)) {
            const v8::String::Utf8Value text(isolate, tryCatch.Exception());
            return std::string("threw: ") + (*text != nullptr ? *text : "?");
        }
        isolate->PerformMicrotaskCheckpoint();
        const auto ns = module->GetModuleNamespace().As<v8::Object>();
        const v8::String::Utf8Value text(isolate,
                                         ns->Get(context, str(exportName)).ToLocalChecked());
        return *text != nullptr ? *text : "";
    });
}

} // namespace

TEST(Modules, BoundModulesImportAsESModules) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::registerModules<EsmTest::GeoMod>(context);
    });
    EXPECT_EQ(
        runModule(fx,
                  "import { Vec, norm2, Mode, ORIGIN_NAME } from 'geo';"
                  "export const out = [new Vec(3, 4).dot(new Vec(1, 1)), norm2(new Vec(3, 4)),"
                  " Mode.On, Mode[1], ORIGIN_NAME].join(',');",
                  "out"),
        "7,25,1,On,origin");
    EXPECT_EQ(runModule(fx, "import * as util from 'geo.util'; export const out = util.twice(21);",
                        "out"),
              "42");
    EXPECT_EQ(runModule(fx,
                        "import * as a from 'geo'; import { Vec } from 'geo';"
                        "export const out = a.Vec === Vec;",
                        "out"),
              "true");
    EXPECT_THAT(runModule(fx, "import { nope } from 'geo'; export const out = 1;", "out"),
                testing::StartsWith("threw: SyntaxError"));
    EXPECT_EQ(runModule(fx, "import x from 'missing'; export const out = 1;", "out"),
              "threw: Error: Cannot find module 'missing'");
}

TEST(Modules, ModuleClassesAreTheGlobalInstallConstructors) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate*, v8::Local<v8::Context> context) {
        Alka::install<EsmTest::GeoMod>(context);
        Alka::registerModules<EsmTest::GeoMod>(context);
    });
    EXPECT_EQ(runModule(fx,
                        "import { Vec } from 'geo'; export const out = Vec === globalThis.geo.Vec;",
                        "out"),
              "true");
}

TEST(Modules, ArePerContext) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::registerModules<EsmTest::GeoMod>(context);
        const auto other = v8::Context::New(isolate);
        const auto spec = v8::String::NewFromUtf8Literal(isolate, "geo");
        EXPECT_FALSE(Alka::resolveModule(context, spec).IsEmpty());
        EXPECT_TRUE(Alka::resolveModule(other, spec).IsEmpty());
    });
}

TEST(Modules, ContextScopeEvalModuleRunsModuleSources) {
    AlkaTest::V8Fixture fx;
    const Alka::ContextScope scope(fx.isolate());
    scope.registerModules<EsmTest::GeoMod>();
    const auto ns =
        scope.evalModule("import { twice } from 'geo.util'; export const v = twice(5);");
    ASSERT_TRUE(ns.has_value());
    EXPECT_EQ(ns->to<Alka::Object>().value().getAs<int>("v").value(), 10);

    const auto failed = scope.evalModule("throw new RangeError('nope');");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "RangeError: nope");
    EXPECT_TRUE(Alka::exceptionValue(failed.error()).valid());

    const auto rejected = scope.evalModule("await Promise.resolve(); throw new Error('later');");
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().message(), "Error: later");
}
