#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace {

template <class Fn> void withValue(AlkaTest::V8Fixture& fx, const std::string& source, Fn&& fn) {
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        v8::Local<v8::Value> raw;
        ASSERT_TRUE(AlkaTest::V8Fixture::compileAndRun(isolate, context, source).ToLocal(&raw));
        Alka::ConvertCtx cx{isolate, context};
        fn(cx, Alka::Value(cx, raw));
    });
}

} // namespace

TEST(Dynamic, ValuesReportTheirJSType) {
    AlkaTest::V8Fixture fx;
    const auto typeOf = [&](const std::string& source) {
        Alka::JsType type{};
        std::string jsTypeof;
        withValue(fx, source, [&](Alka::ConvertCtx&, const Alka::Value& v) {
            type = v.type();
            jsTypeof = std::string(v.typeOf());
        });
        return std::pair{type, jsTypeof};
    };
    EXPECT_EQ(typeOf("undefined"), (std::pair{Alka::JsType::Undefined, std::string("undefined")}));
    EXPECT_EQ(typeOf("null"), (std::pair{Alka::JsType::Null, std::string("object")}));
    EXPECT_EQ(typeOf("true"), (std::pair{Alka::JsType::Boolean, std::string("boolean")}));
    EXPECT_EQ(typeOf("1.5"), (std::pair{Alka::JsType::Number, std::string("number")}));
    EXPECT_EQ(typeOf("10n"), (std::pair{Alka::JsType::BigInt, std::string("bigint")}));
    EXPECT_EQ(typeOf("'s'"), (std::pair{Alka::JsType::String, std::string("string")}));
    EXPECT_EQ(typeOf("Symbol('x')"), (std::pair{Alka::JsType::Symbol, std::string("symbol")}));
    EXPECT_EQ(typeOf("(() => 1)"), (std::pair{Alka::JsType::Function, std::string("function")}));
    EXPECT_EQ(typeOf("[1]"), (std::pair{Alka::JsType::Array, std::string("object")}));
    EXPECT_EQ(typeOf("Promise.resolve(1)"),
              (std::pair{Alka::JsType::Promise, std::string("object")}));
    EXPECT_EQ(typeOf("({})"), (std::pair{Alka::JsType::Object, std::string("object")}));
    withValue(fx, "[]", [](Alka::ConvertCtx&, const Alka::Value& v) {
        EXPECT_TRUE(v.isArray());
        EXPECT_TRUE(v.isObject());
        EXPECT_FALSE(v.isNullish());
    });
    EXPECT_EQ(Alka::Value().type(), Alka::JsType::Invalid);
}

TEST(Dynamic, ObjectKeysPresenceAndRemoval) {
    AlkaTest::V8Fixture fx;
    withValue(fx, "({ a: 1, b: 'two', [Symbol('s')]: 3 })",
              [](Alka::ConvertCtx&, const Alka::Value& v) {
                  const auto object = v.to<Alka::Object>();
                  ASSERT_TRUE(object);
                  EXPECT_EQ(object->keys(), (std::vector<std::string>{"a", "b"}));
                  EXPECT_TRUE(object->has("a").value_or(false));
                  EXPECT_TRUE(object->has("toString").value_or(false));
                  EXPECT_FALSE(object->hasOwn("toString").value_or(true));
                  EXPECT_TRUE(object->remove("a").value_or(false));
                  EXPECT_FALSE(object->has("a").value_or(true));
                  EXPECT_EQ(object->getAs<std::string>("b").value_or(""), "two");
              });
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto made = Alka::Object::create(cx);
        EXPECT_TRUE(made.set("x", 42));
        EXPECT_TRUE(Alka::Object::global(cx).set("made", made));
    });
    EXPECT_EQ(fx.evalInt("made.x"), 42);
}

TEST(Dynamic, CallingFunctionsMethodsAndConstructorsFromCxx) {
    AlkaTest::V8Fixture fx;
    fx.evalString(
        "globalThis.add = (a, b) => a + b;"
        "globalThis.greeter = { name: 'n', greet(who) { return this.name + '>' + who; } };"
        "globalThis.Point = class { constructor(x) { this.x = x; } };"
        "globalThis.fail = () => { throw new RangeError('nope'); }; 'ok'");
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto global = Alka::Object::global(cx);
        EXPECT_EQ(global.get("add")->tryCall<int>(2, 3).value_or(0), 5);
        const auto greeter = global.getAs<Alka::Object>("greeter");
        ASSERT_TRUE(greeter);
        EXPECT_EQ(greeter->tryCallMethod<std::string>("greet", std::string("you")).value_or(""),
                  "n>you");
        EXPECT_FALSE(greeter->tryCallMethod<std::string>("missing"));
        const auto point = global.get("Point")->tryConstruct(7);
        ASSERT_TRUE(point);
        EXPECT_EQ(point->getAs<int>("x").value_or(0), 7);
        const auto failed = global.get("fail")->tryCall();
        ASSERT_FALSE(failed);
        EXPECT_THAT(failed.error().message(), testing::StartsWith("RangeError: nope"));
        EXPECT_TRUE(Alka::exceptionValue(failed.error()).isObject());
    });
}

TEST(Dynamic, ArraysFromCxx) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto array = Alka::Array::create(cx);
        EXPECT_TRUE(array.push(1));
        EXPECT_TRUE(array.push(std::string("two")));
        EXPECT_TRUE(array.set(3, 4.5));
        EXPECT_EQ(array.size().value_or(0), 4);
        EXPECT_EQ(array.getAs<int>(0).value_or(0), 1);
        EXPECT_TRUE(array.get(1)->isString());
        EXPECT_TRUE(array.get(2)->isUndefined());
        EXPECT_TRUE(Alka::Object::global(cx).set("arr", array));
    });
    EXPECT_EQ(fx.evalString("JSON.stringify(arr)"), "[1,\"two\",null,4.5]");
    withValue(fx, "[10, 20]", [](Alka::ConvertCtx&, const Alka::Value& v) {
        const auto array = v.to<Alka::Array>();
        ASSERT_TRUE(array);
        EXPECT_EQ(array->getAs<int>(1).value_or(0), 20);
        EXPECT_EQ(array->getAs<std::string>(0).error().message(),
                  "element 0: expected string, got number");
    });
}

TEST(Dynamic, JSONInAndOutStringAndStrictEquality) {
    AlkaTest::V8Fixture fx;
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto parsed = Alka::parseJson(cx, R"({"a":[1,2],"b":"x"})");
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->toJson().value_or(""), R"({"a":[1,2],"b":"x"})");
        EXPECT_EQ(parsed->toJson(2).value_or(""),
                  "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": \"x\"\n}");
        EXPECT_FALSE(Alka::parseJson(cx, "{oops"));
        EXPECT_FALSE(Alka::Value(cx, v8::Undefined(isolate)).toJson());
        EXPECT_EQ(Alka::Value(cx, v8::Number::New(isolate, 1.5)).toString().value_or(""), "1.5");
        EXPECT_EQ(parsed->toString().value_or(""), "[object Object]");
        const auto symbol =
            Alka::Value(cx, v8::Symbol::New(isolate, Alka::Detail::v8Str(isolate, "tag")));
        EXPECT_EQ(symbol.toString().value_or(""), "Symbol(tag)");
        const Alka::Value& same = *parsed;
        EXPECT_TRUE(same.strictEquals(*parsed));
        EXPECT_FALSE(Alka::parseJson(cx, R"({"a":[1,2],"b":"x"})")->strictEquals(*parsed));
    });
}

TEST(Dynamic, HandleUsedOffItsIsolateThreadReportsNothingButErrors) {
    AlkaTest::V8Fixture fx;
    Alka::Object object;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        object = Alka::Object::create(cx);
    });
    std::thread([&] {
        EXPECT_EQ(object.type(), Alka::JsType::Invalid);
        EXPECT_FALSE(object.keys());
        EXPECT_FALSE(object.tryCallMethod("toString"));
    }).join();
}
