#pragma once

// Showcase bindings for the demo host: both declaration forms, inheritance,
// enums, defaults, metadata -- everything the generated game.d.ts reflects.

#include <Alka/Alka.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace Demo {

// --- in-class form: the binding lives inside the type -------------------------
struct Vec2 {
    Vec2() = default;
    Vec2(double x, double y) : mX(x), mY(y) {}
    double mX = 0;
    double mY = 0;

    static constexpr auto alkaMembers = Alka::members(
        Alka::ctor<>(), Alka::ctor<double, double>(Alka::arg("x"), Alka::arg("y")),
        Alka::prop<&Vec2::mX>(), Alka::prop<&Vec2::mY>());
};

// --- plain C++ classes, bound non-intrusively below ---------------------------
struct Entity {
    explicit Entity(std::string name) : mName(std::move(name)) {}
    virtual ~Entity() = default;
    Entity(const Entity&) = default;
    Entity& operator=(const Entity&) = default;

    std::string mName;
    int mHp = 100;
    Vec2 mPos;

    std::string describe() const { return mName + " (hp " + std::to_string(mHp) + ")"; }
    void update(double dt) { mHp -= static_cast<int>(dt * 10.0); }
};

struct Player : Entity {
    Player(std::string name, int level) : Entity(std::move(name)), mLevel(level) {}
    int mLevel = 1;
    std::string title() const { return "Lv" + std::to_string(mLevel) + " " + mName; }
};

enum class Color { Red, Green = 5, Blue };

inline double length(const Vec2& v) { return std::sqrt(v.mX * v.mX + v.mY * v.mY); }
inline std::string describeAny(const Entity& e) { return e.describe(); }
inline Player spawn(const std::string& name, int level) { return Player(name, level); }
inline std::vector<int> range(int n) {
    std::vector<int> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(i);
    }
    return out;
}

inline std::string think(Entity& e) { return e.mName + " ponders..."; }

struct GameAiMod;
struct GameMod;

}  // namespace Demo

template<>
struct Alka::Bind<Demo::Entity> : Alka::Class<Demo::Entity> {
    static constexpr auto doc = "Base entity living in the scene.";
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string>(Alka::arg("name").doc("display name")),
        Alka::prop<&Demo::Entity::mName>(),
        Alka::prop<&Demo::Entity::mHp>(Alka::readonly),
        Alka::prop<&Demo::Entity::mPos>(),
        Alka::method<&Demo::Entity::describe>(),
        Alka::method<&Demo::Entity::update>(Alka::doc("advance the simulation"),
                                            Alka::arg("dt").doc("delta seconds") = 0.016));
};

template<>
struct Alka::Bind<Demo::Player> : Alka::Class<Demo::Player> {
    using Extends = Demo::Entity;
    static constexpr auto doc = "A controllable player.";
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("level") = 1),
        Alka::prop<&Demo::Player::mLevel>(Alka::meta<"category">("progression")),
        Alka::method<&Demo::Player::title>());
};

template<>
struct Alka::Bind<Demo::GameAiMod> : Alka::Module<"game.ai"> {
    static constexpr auto doc = "AI helpers";
    static constexpr auto members = Alka::members(Alka::fn<&Demo::think>(Alka::arg("e")));
};

template<>
struct Alka::Bind<Demo::GameMod> : Alka::Module<"game"> {
    static constexpr auto doc = "Alka demo module";
    static constexpr auto members = Alka::members(
        Alka::type<Demo::Vec2>, Alka::type<Demo::Entity>, Alka::type<Demo::Player>,
        Alka::enumType<Demo::Color>(),
        Alka::fn<&Demo::length>(Alka::arg("v")),
        Alka::fn<&Demo::describeAny>(Alka::arg("e")),
        Alka::fn<&Demo::spawn>(Alka::arg("name"), Alka::arg("level") = 1),
        Alka::fn<&Demo::range>(Alka::arg("n")),
        Alka::var<"VERSION">(3),
        Alka::submodule<Demo::GameAiMod>);
};
