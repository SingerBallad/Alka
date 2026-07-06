#include <Alka/Alka.hpp>
#include <Alka/Introspect.hpp>
#include <Alka/Ts/Generator.hpp>

#include <doctest/doctest.h>

#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace TsGenTest {

struct Vec2 {
    Vec2() = default;
    Vec2(double x, double y) : mX(x), mY(y) {}
    double mX = 0;
    double mY = 0;
};

struct Entity {
    explicit Entity(std::string name) : mName(std::move(name)) {}
    virtual ~Entity() = default;
    Entity(const Entity&) = default;
    Entity& operator=(const Entity&) = default;

    std::string mName;
    int mHp = 100;
    Vec2 mPos;

    std::string describe() const { return mName; }
    void update(double) {}
    int getShield() const { return mShield; }
    void setShield(int v) { mShield = v; }
    static int count() { return 0; }

private:
    int mShield = 0;
};

struct Player : Entity {
    Player(std::string name, int level) : Entity(std::move(name)), mLevel(level) {}
    int mLevel = 1;
    std::string title() const { return mName; }
    std::string cast() { return "zap"; }
    std::string cast(int power) { return std::to_string(power); }
};

enum class Color { Red, Green = 5, Blue };

inline Player* find(const std::string&) { return nullptr; }
inline std::vector<Entity*> all() { return {}; }
inline int sum(const std::vector<int>& v) { return static_cast<int>(v.size()); }
inline void onTick(std::function<void(double)>) {}
inline std::optional<int> tryParse(const std::string&) { return std::nullopt; }
inline Player spawn(const std::string&, int) { return Player("p", 1); }

struct GameAiMod;
struct GameMod;

inline std::string think(Entity& e) { return e.describe(); }

}  // namespace TsGenTest

template<>
struct Alka::Bind<TsGenTest::Vec2> : Alka::Class<TsGenTest::Vec2> {
    static constexpr auto doc = "2D vector value type";
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::ctor<double, double>(Alka::arg("x"), Alka::arg("y")),
        Alka::prop<&TsGenTest::Vec2::mX>(), Alka::prop<&TsGenTest::Vec2::mY>());
};

template<>
struct Alka::Bind<TsGenTest::Entity> : Alka::Class<TsGenTest::Entity> {
    static constexpr auto doc = "Base entity.\nLives in the scene graph.";
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string>(Alka::arg("name").doc("display name")),
        Alka::prop<&TsGenTest::Entity::mName>(),
        Alka::prop<&TsGenTest::Entity::mHp>(Alka::readonly),
        Alka::prop<&TsGenTest::Entity::mPos>(),
        Alka::prop<"shield", &TsGenTest::Entity::getShield, &TsGenTest::Entity::setShield>(),
        Alka::method<&TsGenTest::Entity::describe>(),
        Alka::method<&TsGenTest::Entity::update>(Alka::doc("advance the simulation"),
                                                 Alka::arg("dt").doc("delta seconds") = 0.016),
        Alka::staticMethod<&TsGenTest::Entity::count>(),
        Alka::var<"MAX_HP">(100));
};

template<>
struct Alka::Bind<TsGenTest::Player> : Alka::Class<TsGenTest::Player> {
    using Extends = TsGenTest::Entity;
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("level") = 1),
        Alka::prop<&TsGenTest::Player::mLevel>(Alka::meta<"category">("progression")),
        Alka::method<&TsGenTest::Player::title>(Alka::deprecated("use describe()")),
        Alka::method<"cast", static_cast<std::string (TsGenTest::Player::*)()>(&TsGenTest::Player::cast),
                     static_cast<std::string (TsGenTest::Player::*)(int)>(&TsGenTest::Player::cast)>());
};

template<>
struct Alka::Bind<TsGenTest::GameAiMod> : Alka::Module<"game.ai"> {
    static constexpr auto doc = "AI helpers";
    static constexpr auto members =
        Alka::members(Alka::fn<&TsGenTest::think>(Alka::arg("e")));
};

template<>
struct Alka::Bind<TsGenTest::GameMod> : Alka::Module<"game"> {
    static constexpr auto doc = "Game core module";
    static constexpr auto members = Alka::members(
        Alka::type<TsGenTest::Vec2>, Alka::type<TsGenTest::Entity>,
        Alka::type<TsGenTest::Player>, Alka::enumType<TsGenTest::Color>(),
        Alka::fn<&TsGenTest::find>(Alka::arg("name")),
        Alka::fn<&TsGenTest::all>(),
        Alka::fn<&TsGenTest::sum>(Alka::arg("values")),
        Alka::fn<&TsGenTest::onTick>(Alka::arg("cb").doc("called every frame")),
        Alka::fn<&TsGenTest::tryParse>(Alka::arg("text")),
        Alka::fn<&TsGenTest::spawn>(Alka::arg("name"), Alka::arg("level") = 1,
                                    Alka::meta<"category">("factory")),
        Alka::var<"VERSION">(3),
        Alka::var<"TITLE">("Alka Demo"),
        Alka::submodule<TsGenTest::GameAiMod>);
};

namespace {

std::string normalize(std::string text) {
    std::erase(text, '\r');
    return text;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

void compareWithGolden(const std::string& actual, const std::string& goldenName) {
    const std::string goldenDir = ALKA_TEST_GOLDEN_DIR;
    writeFile(goldenDir + "/" + goldenName + ".actual.d.ts", actual);
    const std::string golden = normalize(readFile(goldenDir + "/" + goldenName + ".d.ts"));
    REQUIRE_MESSAGE(!golden.empty(),
                    "golden file missing; review " << goldenName
                                                   << ".actual.d.ts and run tools/UpdateGolden.py");
    CHECK_MESSAGE(normalize(actual) == golden,
                  "generated dts diverged from Golden/" << goldenName
                                                        << ".d.ts; review the .actual.d.ts file");
}

}  // namespace

TEST_CASE("dts golden: global namespace style") {
    compareWithGolden(Alka::generateDts<TsGenTest::GameMod>(), "GameGlobal");
}

TEST_CASE("dts golden: es module style") {
    Alka::TsGenOptions options;
    options.mStyle = Alka::TsStyle::EsModule;
    compareWithGolden(Alka::generateDts<TsGenTest::GameMod>(options), "GameModule");
}

TEST_CASE("dts generation is deterministic") {
    CHECK(Alka::generateDts<TsGenTest::GameMod>() == Alka::generateDts<TsGenTest::GameMod>());
}

TEST_CASE("meta renderer hooks rewrite custom tags") {
    Alka::TsGenOptions options;
    options.mMetaRenderers["category"] = [](const Alka::MetaValue& value) {
        return "@category " + std::string(value.mString);
    };
    const std::string dts = Alka::generateDts<TsGenTest::GameMod>(options);
    CHECK(dts.find("@category factory") != std::string::npos);
    CHECK(dts.find("@category progression") != std::string::npos);
    CHECK(dts.find("@alka category") == std::string::npos);
}
