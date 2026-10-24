#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace DynTest {

struct Config {
    inline static bool sDebug = false;
    inline static const int sLimit = 64;
    inline static int sLevel = 1;

    std::map<std::string, std::string> mValues{{"host", "localhost"}};

    [[nodiscard]] std::optional<std::string> find(const std::string& key) const {
        const auto it = mValues.find(key);
        return it != mValues.end() ? std::optional(it->second) : std::nullopt;
    }
    void put(const std::string& key, std::string value) { mValues[key] = std::move(value); }
    bool erase(const std::string& key) { return mValues.erase(key) > 0; }
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& key : mValues | std::views::keys) {
            out.push_back(key);
        }
        return out;
    }
    [[nodiscard]] std::string describe() const { return "config"; }
};
inline int getLevel() {
    return Config::sLevel;
}
inline void setLevel(int level) {
    Config::sLevel = level;
}
inline std::string version() {
    return "1.2";
}

struct SubConfig : Config {};

struct Env {
    [[nodiscard]] const std::string* lookup(const std::string& key) const {
        static const std::string path = "/bin";
        return key == "PATH" ? &path : nullptr;
    }
};

struct Matcher {
    std::string mNeedle;
    [[nodiscard]] bool test(const std::string& text) const {
        return text.find(mNeedle) != std::string::npos;
    }
};

struct Mod;

} // namespace DynTest

template <> struct Alka::Bind<DynTest::Config> : Class<DynTest::Config> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&DynTest::Config::describe>(),
        Alka::staticProp<&DynTest::Config::sDebug>(), Alka::staticProp<&DynTest::Config::sLimit>(),
        Alka::staticProp<"level", &DynTest::getLevel, &DynTest::setLevel>(),
        Alka::staticProp<"version", &DynTest::version>(),
        Alka::namedProperties<&DynTest::Config::find, &DynTest::Config::put, nullptr,
                              &DynTest::Config::erase, &DynTest::Config::names>());
};

template <> struct Alka::Bind<DynTest::SubConfig> : Class<DynTest::SubConfig> {
    using Extends = DynTest::Config;
    static constexpr auto members = Alka::members(Alka::ctor<>());
};

template <> struct Alka::Bind<DynTest::Env> : Class<DynTest::Env> {
    static constexpr auto lifetime = Lifetime::Static;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::namedProperties<&DynTest::Env::lookup>(policy::copy));
};

template <> struct Alka::Bind<DynTest::Matcher> : Class<DynTest::Matcher> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&DynTest::Matcher::mNeedle>(),
                      Alka::asFunction<&DynTest::Matcher::test>(arg("text")));
};

template <> struct Alka::Bind<DynTest::Mod> : Module<"dyn"> {
    static constexpr auto members =
        Alka::members(Alka::type<DynTest::Config>, Alka::type<DynTest::SubConfig>,
                      Alka::type<DynTest::Env>, Alka::type<DynTest::Matcher>);
};

namespace {

struct DynFixture : AlkaTest::V8Fixture {
    DynFixture() {
        DynTest::Config::sDebug = false;
        DynTest::Config::sLevel = 1;
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<DynTest::Mod>(context);
        });
    }
};

} // namespace

TEST(DynamicMembers, StaticPropertiesReadAndWriteStaticState) {
    DynFixture fx;
    EXPECT_FALSE(fx.evalBool("dyn.Config.debug"));
    fx.evalString("dyn.Config.debug = true; 'ok'");
    EXPECT_TRUE(DynTest::Config::sDebug); // the variable itself
    EXPECT_EQ(fx.evalInt("dyn.Config.limit"), 64);
    fx.evalString("dyn.Config.limit = 1; 'ok'");
    EXPECT_EQ(fx.evalInt("dyn.Config.limit"), 64);
    fx.evalString("dyn.Config.level = 5; 'ok'");
    EXPECT_EQ(DynTest::Config::sLevel, 5);
    EXPECT_EQ(fx.evalString("dyn.Config.version"), "1.2");
    EXPECT_EQ(fx.evalInt("dyn.SubConfig.level"), 5);
    EXPECT_EQ(fx.evalString("try { dyn.Config.level = 'x' } catch (e) { e.message }"),
              "Config.level: expected number, got string");
}

TEST(DynamicMembers, NamedPropertiesServeDynamicNames) {
    DynFixture fx;
    fx.evalString("globalThis.c = new dyn.Config(); 'ok'");
    EXPECT_EQ(fx.evalString("c.host"), "localhost");
    EXPECT_TRUE(fx.evalBool("c.missing === undefined && !('missing' in c) && ('host' in c)"));
    fx.evalString("c.port = '8080'; 'ok'");
    EXPECT_EQ(fx.evalString("c.port"), "8080");
    EXPECT_EQ(fx.evalString("Object.keys(c).sort().join(',')"), "host,port");
    EXPECT_TRUE(fx.evalBool("delete c.port"));
    EXPECT_TRUE(fx.evalBool("c.port === undefined"));
    // the class's own members win over dynamic names
    EXPECT_EQ(fx.evalString("c.describe()"), "config");
    EXPECT_EQ(fx.evalString("try { c.port = 5 } catch (e) { e.message }"),
              "Config.[name]: expected string, got number");
    // inherited by bound subclasses
    EXPECT_EQ(fx.evalString("new dyn.SubConfig().host"), "localhost");
}

TEST(DynamicMembers, ReadOnlyNamedPropertiesRefuseWritesToTheirNames) {
    DynFixture fx;
    fx.evalString("globalThis.e = new dyn.Env(); 'ok'");
    EXPECT_EQ(fx.evalString("e.PATH"), "/bin");
    EXPECT_EQ(fx.evalString("try { e.PATH = 'x'; 'no' } catch (err) { err.message }"),
              "Env.[name]: 'PATH' is read-only");
    fx.evalString("e.other = 1; 'ok'");
    EXPECT_EQ(fx.evalInt("e.other"), 1);
}

TEST(DynamicMembers, CallableInstancesRunTheirDeclaredFunction) {
    DynFixture fx;
    fx.evalString("globalThis.m = new dyn.Matcher(); m.needle = 'ab'; 'ok'");
    EXPECT_TRUE(fx.evalBool("m('xxabxx')"));
    EXPECT_FALSE(fx.evalBool("m('nope')"));
    EXPECT_TRUE(fx.evalBool("['ab', 'cd', 'zab'].filter(m).length === 2"));
    EXPECT_EQ(fx.evalString("try { m(1) } catch (e) { e.message }"),
              "arg 1 (text) of Matcher.[call]: expected string, got number");
}

TEST(DynamicMembers, StaticNamedAndCallableMembersInTheDts) {
    const auto dts = Alka::generateDts<DynTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("static debug: boolean;"));
    EXPECT_THAT(dts, testing::HasSubstr("static readonly limit: number;"));
    EXPECT_THAT(dts, testing::HasSubstr("static level: number;"));
    EXPECT_THAT(dts, testing::HasSubstr("static readonly version: string;"));
    EXPECT_THAT(dts, testing::HasSubstr("/** dynamic properties: string */"));
    EXPECT_THAT(dts, testing::HasSubstr("[name: string]: unknown;"));
    EXPECT_THAT(dts,
                testing::HasSubstr("interface Matcher {\n        (text: string): boolean;\n    }"));
}
