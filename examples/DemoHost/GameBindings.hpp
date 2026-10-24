#pragma once
#include <Alka/Alka.hpp>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Demo {

struct Vec2 {
    Vec2() = default;
    Vec2(double x, double y) : mX(x), mY(y) {}
    double mX = 0;
    double mY = 0;

    static constexpr auto alkaNaming = Alka::memberPrefixes;
    static constexpr auto alkaMembers =
        Alka::members(Alka::ctor<>(), Alka::ctor<double, double>(Alka::arg("x"), Alka::arg("y")),
                      Alka::prop<&Vec2::mX>(), Alka::prop<&Vec2::mY>());
};

struct Entity {
    explicit Entity(std::string name) : mName(std::move(name)) {}
    virtual ~Entity() = default;
    Entity(const Entity&) = default;
    Entity& operator=(const Entity&) = default;

    std::string mName;
    int mHp = 100;
    Vec2 mPos;

    virtual std::string describe() const { return mName + " (hp " + std::to_string(mHp) + ")"; }
    void update(double dt) { mHp -= static_cast<int>(dt * 10.0); }
};

struct JsEntity : Entity, Alka::Overridable<Entity> {
    using Entity::Entity;
    std::string describe() const override { return ALKA_OVERRIDE(std::string, Entity, describe); }
};

struct GameError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Player : Entity {
    Player(std::string name, int level) : Entity(std::move(name)), mLevel(level) {}
    int mLevel = 1;
    std::string title() const { return "Lv" + std::to_string(mLevel) + " " + mName; }
};

enum class Color { Red, Green = 5, Blue };

inline double length(const Vec2& v) {
    return std::sqrt(v.mX * v.mX + v.mY * v.mY);
}
inline std::string describeAny(const Entity& e) {
    return e.describe();
}
inline Player spawn(const std::string& name, int level) {
    return {name, level};
}
inline std::vector<int> range(int n) {
    std::vector<int> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(i);
    }
    return out;
}

inline std::string think(Entity& e) {
    return e.mName + " ponders...";
}

inline void forbidden() {
    throw GameError("you shall not pass");
}

inline Alka::Task<int> slowSquare(int v) {
    co_await Alka::offload([] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
    co_return v* v;
}

inline Alka::Promise<std::string> fetchGreeting(std::string name) {
    auto promise = Alka::Promise<std::string>::create();
    const Alka::Dispatcher dispatcher(v8::Isolate::GetCurrent());
    dispatcher.retain();
    std::thread([settler = promise.settler(), dispatcher, name = std::move(name)] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // "network"
        (void)settler.resolve("hello, " + name);
        dispatcher.release();
    }).detach();
    return promise;
}

struct GameAiMod;
struct GameMod;

} // namespace Demo

template <> struct Alka::Bind<Demo::Entity> : Class<Demo::Entity> {
    static constexpr auto naming = Alka::memberPrefixes;
    using Trampoline = Demo::JsEntity;
    static constexpr auto doc = "Base entity living in the scene.";
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string>(arg("name").doc("display name")),
        Alka::prop<&Demo::Entity::mName>(), Alka::prop<&Demo::Entity::mHp>(readonly),
        Alka::prop<&Demo::Entity::mPos>(), Alka::method<"describe", &Demo::Entity::describe>(),
        Alka::method<&Demo::Entity::update>(Alka::doc("advance the simulation"),
                                            arg("dt").doc("delta seconds") = 0.016));
};

template <> struct Alka::Bind<Demo::Player> : Class<Demo::Player> {
    static constexpr auto naming = Alka::memberPrefixes;
    using Extends = Demo::Entity;
    static constexpr auto doc = "A controllable player.";
    static constexpr auto members =
        Alka::members(Alka::ctor<std::string, int>(arg("name"), arg("level") = 1),
                      Alka::prop<&Demo::Player::mLevel>(Alka::meta<"category">("progression")),
                      Alka::method<&Demo::Player::title>());
};

template <> struct Alka::Bind<Demo::GameAiMod> : Module<"game.ai"> {
    static constexpr auto doc = "AI helpers";
    static constexpr auto members = Alka::members(Alka::fn<&Demo::think>(arg("e")));
};

template <> struct Alka::Bind<Demo::GameMod> : Module<"game"> {
    static constexpr auto doc = "Alka demo module";
    static constexpr auto members = Alka::members(
        Alka::type<Demo::Vec2>, Alka::type<Demo::Entity>, Alka::type<Demo::Player>,
        Alka::enumType<Demo::Color>(), Alka::fn<&Demo::length>(arg("v")),
        Alka::fn<&Demo::describeAny>(arg("e")),
        Alka::fn<&Demo::spawn>(arg("name"), arg("level") = 1), Alka::fn<&Demo::range>(arg("n")),
        Alka::fn<&Demo::forbidden>(), Alka::fn<&Demo::slowSquare>(arg("v")),
        Alka::fn<&Demo::fetchGreeting>(arg("name")), Alka::error<"GameError", Demo::GameError>(),
        Alka::var<"VERSION">(3), Alka::submodule<Demo::GameAiMod>);
};
