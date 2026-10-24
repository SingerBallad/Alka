#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace OverrideTest {

class Animal {
public:
    explicit Animal(std::string name) : mName(std::move(name)) {}
    virtual ~Animal() = default;
    Animal(const Animal&) = default;
    Animal& operator=(const Animal&) = default;

    virtual std::string speak() { return mName + " makes a sound"; }
    [[nodiscard]] virtual int legs() const { return 4; }
    virtual std::string greet(const Animal& other) { return "hi " + other.mName; }

    std::string mName;
};

struct JsAnimal : Animal, Alka::Overridable<Animal> {
    using Animal::Animal;
    std::string speak() override { return ALKA_OVERRIDE(std::string, Animal, speak); }
    [[nodiscard]] int legs() const override { return ALKA_OVERRIDE(int, Animal, legs); }
    std::string greet(const Animal& other) override {
        return ALKA_OVERRIDE(std::string, Animal, greet, other);
    }
};

class Shape {
public:
    virtual ~Shape() = default;
    [[nodiscard]] virtual double area() const = 0;
};

struct JsShape : Shape, Alka::Overridable<Shape> {
    [[nodiscard]] double area() const override { return ALKA_OVERRIDE_PURE(double, Shape, area); }
};

class Bird {
public:
    virtual ~Bird() = default;
    virtual std::string sing() { return "la"; }
    [[nodiscard]] virtual int wings() const { return 2; }
};

struct JsBird : Bird, Alka::Overridable<Bird> {
    std::string sing() override {
        return Alka::overrideOr<std::string, "chirp">(this, [&] { return Bird::sing(); });
    }
    [[nodiscard]] int wings() const override {
        static const std::string jsName = "wingCount";
        return Alka::overrideOr<int>(this, jsName, [&] { return Bird::wings(); });
    }
};

class Fish {
public:
    virtual ~Fish() = default;
    virtual std::string swim() { return "fish"; }
    int mFins = 2;
};

struct JsFish : Alka::Overridable<Fish>, Fish {
    std::string swim() override { return ALKA_OVERRIDE(std::string, Fish, swim); }
};

inline std::string swimOf(Fish& f) {
    return f.swim();
}
inline Fish* echoFish(Fish* f) {
    return f;
}
inline std::unique_ptr<Fish> hatch() {
    return std::make_unique<JsFish>();
}

inline std::string birdSong(Bird& b) {
    return b.sing();
}
inline int birdWings(const Bird& b) {
    return b.wings();
}

inline std::string chorus(Animal& a) {
    return a.speak();
}
inline int legsOf(const Animal& a) {
    return a.legs();
}
inline std::string greetBoth(Animal& a, const Animal& b) {
    return a.greet(b);
}
inline double areaOf(const Shape& s) {
    return s.area();
}
inline Animal* echo(Animal* a) {
    return a;
}

inline std::shared_ptr<Animal> gKept;
inline void keep(std::shared_ptr<Animal> a) {
    gKept = std::move(a);
}
inline std::string speakKept() {
    return gKept ? gKept->speak() : "none";
}

struct Mod;

} // namespace OverrideTest

template <> struct Alka::Bind<OverrideTest::Animal> : Class<OverrideTest::Animal> {
    static constexpr auto naming = memberPrefixes;
    using Trampoline = OverrideTest::JsAnimal;
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string>(arg("name")), Alka::method<"speak", &OverrideTest::Animal::speak>(),
        Alka::method<"legs", &OverrideTest::Animal::legs>(),
        Alka::method<"greet", &OverrideTest::Animal::greet>(Alka::arg("other")),
        Alka::prop<&OverrideTest::Animal::mName>());
};

template <> struct Alka::Bind<OverrideTest::Shape> : Class<OverrideTest::Shape> {
    using Trampoline = OverrideTest::JsShape;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"area", &OverrideTest::Shape::area>());
};

template <> struct Alka::Bind<OverrideTest::Bird> : Class<OverrideTest::Bird> {
    using Trampoline = OverrideTest::JsBird;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"chirp", &OverrideTest::Bird::sing>(),
                      Alka::method<"wingCount", &OverrideTest::Bird::wings>());
};

template <> struct Alka::Bind<OverrideTest::Fish> : Class<OverrideTest::Fish> {
    static constexpr auto naming = memberPrefixes;
    using Trampoline = OverrideTest::JsFish;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"swim", &OverrideTest::Fish::swim>(),
                      Alka::prop<&OverrideTest::Fish::mFins>());
};

template <> struct Alka::Bind<OverrideTest::Mod> : Module<"zoo"> {
    static constexpr auto members = Alka::members(
        Alka::type<OverrideTest::Animal>, Alka::type<OverrideTest::Shape>,
        Alka::type<OverrideTest::Bird>, Alka::fn<&OverrideTest::birdSong>(),
        Alka::fn<&OverrideTest::birdWings>(), Alka::fn<&OverrideTest::chorus>(),
        Alka::fn<&OverrideTest::legsOf>(), Alka::fn<&OverrideTest::greetBoth>(),
        Alka::fn<&OverrideTest::areaOf>(), Alka::fn<&OverrideTest::echo>(policy::borrow),
        Alka::fn<&OverrideTest::keep>(), Alka::fn<&OverrideTest::speakKept>(),
        Alka::type<OverrideTest::Fish>, Alka::fn<&OverrideTest::swimOf>(),
        Alka::fn<&OverrideTest::echoFish>(Alka::policy::borrow), Alka::fn<&OverrideTest::hatch>());
};

namespace {

struct OverrideFixture : AlkaTest::V8Fixture {
    OverrideFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<OverrideTest::Mod>(context);
        });
        evalString("globalThis.Dog = class extends zoo.Animal { speak() { return 'woof'; } };"
                   "globalThis.Cat = class extends zoo.Animal {"
                   "  speak() { return 'meow + ' + super.speak(); } }; 'ok'");
    }
};

} // namespace

TEST(Override, TrampolineWhoseOverridableBaseComesFirst) {
    OverrideFixture fx;
    fx.evalString("globalThis.Shark = class extends zoo.Fish { swim() { return 'shark'; } }; 'ok'");
    EXPECT_EQ(fx.evalString("zoo.swimOf(new Shark())"), "shark");
    EXPECT_TRUE(fx.evalBool("(() => { const s = new Shark(); return zoo.echoFish(s) === s; })()"));
    EXPECT_EQ(fx.evalInt("new Shark().fins"), 2);
    EXPECT_EQ(fx.evalString("zoo.hatch().swim()"), "fish");
    EXPECT_EQ(fx.evalInt("zoo.hatch().fins"), 2);
}

TEST(Override, CxxVirtualCallsReachJSOverrides) {
    OverrideFixture fx;
    EXPECT_EQ(fx.evalString("zoo.chorus(new Dog('rex'))"), "woof");
    EXPECT_EQ(fx.evalString("zoo.chorus(new zoo.Animal('a'))"), "a makes a sound");
    EXPECT_EQ(fx.evalString("new Dog('rex').speak()"), "woof");
    // const virtuals too
    EXPECT_EQ(
        fx.evalInt("zoo.legsOf(new (class extends zoo.Animal { legs() { return 0; } })('s'))"), 0);
    EXPECT_EQ(fx.evalInt("zoo.legsOf(new zoo.Animal('a'))"), 4);
}

TEST(Override, MethodJSDoesNotOverrideRunsTheCxxBasePrototypePatchesStillCount) {
    OverrideFixture fx;
    EXPECT_EQ(fx.evalString("zoo.chorus(new (class extends zoo.Animal {})('plain'))"),
              "plain makes a sound");
    EXPECT_EQ(fx.evalInt("zoo.legsOf(new (class extends zoo.Animal {})('p'))"), 4);
    fx.evalString("zoo.Animal.prototype.speak = function () { return 'patched ' + this.name; };"
                  " 'ok'");
    EXPECT_EQ(fx.evalString("zoo.chorus(new (class extends zoo.Animal {})('p'))"), "patched p");
    fx.evalString("zoo.Animal.prototype.speak = (function () { return 'bound ' + this.n; })"
                  ".bind({ n: 'x' }); 'ok'");
    EXPECT_EQ(fx.evalString("zoo.chorus(new (class extends zoo.Animal {})('p'))"), "bound x");
}

TEST(Override, SuperCallsRunTheCxxBaseWithoutRecursing) {
    OverrideFixture fx;
    EXPECT_EQ(fx.evalString("zoo.chorus(new Cat('tom'))"), "meow + tom makes a sound");
    EXPECT_EQ(fx.evalString("new Cat('tom').speak()"), "meow + tom makes a sound");
}

TEST(Override, PerInstanceOverridesAndBorrowedArguments) {
    OverrideFixture fx;
    EXPECT_EQ(fx.evalString("(() => { const a = new zoo.Animal('x'); a.speak = () => 'patched';"
                            " return zoo.chorus(a); })()"),
              "patched");
    EXPECT_EQ(
        fx.evalString("zoo.greetBoth(new (class extends zoo.Animal {"
                      "  greet(o) { return 'hello ' + o.name; } })('f'), new zoo.Animal('b'))"),
        "hello b");
}

TEST(Override, PureVirtualsJSImplementsThemOrCallingThrows) {
    OverrideFixture fx;
    EXPECT_EQ(
        fx.evalDouble("zoo.areaOf(new (class extends zoo.Shape {"
                      "  constructor(s) { super(); this.s = s; } area() { return this.s * this.s; }"
                      "})(3))"),
        9.0);
    EXPECT_EQ(fx.evalString("try { zoo.areaOf(new zoo.Shape()) } catch (e) { e.message }"),
              "Shape.area is not implemented");
}

TEST(Override, OverrideOrJSNamesDifferingFromCxxFixedOrRunTime) {
    OverrideFixture fx;
    EXPECT_EQ(fx.evalString("zoo.birdSong(new zoo.Bird())"), "la");
    EXPECT_EQ(fx.evalString("zoo.birdSong(new (class extends zoo.Bird {"
                            "  chirp() { return 'tweet'; } })())"),
              "tweet");
    EXPECT_EQ(fx.evalString("zoo.birdSong(new (class extends zoo.Bird {"
                            "  chirp() { return 'x' + super.chirp(); } })())"),
              "xla");
    EXPECT_EQ(fx.evalInt("zoo.birdWings(new zoo.Bird())"), 2);
    EXPECT_EQ(fx.evalInt("zoo.birdWings(new (class extends zoo.Bird {"
                         "  wingCount() { return 4; } })())"),
              4);
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context>) {
        const auto second = v8::Context::New(isolate);
        const v8::Context::Scope scope(second);
        Alka::install<OverrideTest::Mod>(second);
        v8::Local<v8::Value> result;
        ASSERT_TRUE(
            AlkaTest::V8Fixture::compileAndRun(
                isolate, second,
                "zoo.birdSong(new (class extends zoo.Bird { chirp() { return 'again'; } })())")
                .ToLocal(&result));
        EXPECT_EQ(std::string(*v8::String::Utf8Value(isolate, result)), "again");
    });
}

TEST(Override, FailuresSurfaceProperly) {
    OverrideFixture fx;
    EXPECT_TRUE(fx.evalBool(
        "(() => { const err = new RangeError('bad');"
        " try { zoo.chorus(new (class extends zoo.Animal { speak() { throw err; } })('b')); }"
        " catch (e) { return e === err; } return false; })()"));
    EXPECT_EQ(
        fx.evalString(
            "try { zoo.chorus(new (class extends zoo.Animal { speak() { return 5; } })('w')) }"
            " catch (e) { e.message }"),
        "the JS override of speak returned an incompatible value: expected string, got number");
}

TEST(Override, PinnedInstanceKeepsItsOverridesIdentityHolds) {
    OverrideFixture fx;
    EXPECT_TRUE(fx.evalBool("(() => { const d = new Dog('d'); return zoo.echo(d) === d; })()"));
    fx.evalString("zoo.keep(new Dog('kept')); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(fx.evalString("zoo.speakKept()"), "woof");
    OverrideTest::gKept.reset();
}
