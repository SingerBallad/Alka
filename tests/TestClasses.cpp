#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <doctest/doctest.h>

#include <string>

namespace ClassTest {

class Sword {
public:
    Sword(std::string name, int damage) : mName(std::move(name)), mDamage(damage) { ++sForged; }

    std::string mName;
    int mDamage = 1;

    [[nodiscard]] int strike(const int bonus) const { return mDamage + bonus; }
    void rename(const std::string& name) { mName = name; }
    [[nodiscard]] auto describe() const -> std::string { return mName + " (" + std::to_string(mDamage) + ")"; }

    [[nodiscard]] auto getSecret() const -> int { return mSecret; }
    void setSecret(int v) { mSecret = v * 2; }

    static int forged() { return sForged; }

    inline static int sForged = 0;

private:
    int mSecret = 0;
};

// Overload material: two constructors, one overloaded method.
class Potion {
public:
    Potion() : mLabel("unnamed"), mDoses(1) {}
    Potion(std::string label, int doses) : mLabel(std::move(label)), mDoses(doses) {}

    std::string mLabel;
    int mDoses = 0;

    std::string sip() { return mLabel + ":1"; }
    std::string sip(int n) { return mLabel + ":" + std::to_string(n); }
};

struct ForgeMod;

}  // namespace ClassTest

template<>
struct Alka::Bind<ClassTest::Sword> : Alka::Class<ClassTest::Sword> {
    static constexpr auto doc = "a pointy tool";
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("damage")),
        Alka::method<&ClassTest::Sword::strike>(Alka::arg("bonus")),
        Alka::method<&ClassTest::Sword::rename>(Alka::arg("name")),
        Alka::method<&ClassTest::Sword::describe>(),
        Alka::method<"shout", [](const ClassTest::Sword& self) { return self.mName + "!!!"; }>(),
        Alka::prop<&ClassTest::Sword::mName>(),
        Alka::prop<&ClassTest::Sword::mDamage>(Alka::readonly),
        Alka::prop<"secret", &ClassTest::Sword::getSecret, &ClassTest::Sword::setSecret>(),
        Alka::staticMethod<&ClassTest::Sword::forged>(),
        Alka::var<"MAX_DAMAGE">(9000));
};

template<>
struct Alka::Bind<ClassTest::Potion> : Alka::Class<ClassTest::Potion> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::ctor<std::string, int>(Alka::arg("label"), Alka::arg("doses")),
        Alka::method<"sip", static_cast<std::string (ClassTest::Potion::*)()>(&ClassTest::Potion::sip),
                     static_cast<std::string (ClassTest::Potion::*)(int)>(&ClassTest::Potion::sip)>(),
        Alka::prop<&ClassTest::Potion::mDoses>());
};

template<>
struct Alka::Bind<ClassTest::ForgeMod> : Alka::Module<"forge"> {
    static constexpr auto members =
        Alka::members(Alka::type<ClassTest::Sword>, Alka::type<ClassTest::Potion>);
};

namespace {

struct ForgeFixture : AlkaTest::V8Fixture {
    ForgeFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<ClassTest::ForgeMod>(context);
        });
    }
};

}  // namespace

TEST_CASE("classes construct and call methods from JS") {
    ForgeFixture fx;
    CHECK(fx.evalString("typeof forge.Sword") == "function");
    CHECK(fx.evalInt("globalThis.s = new forge.Sword('Excalibur', 10); s.strike(5)") == 15);
    CHECK(fx.evalString("s.describe()") == "Excalibur (10)");
    CHECK(fx.evalString("s.shout()") == "Excalibur!!!");
    fx.evalString("s.rename('Durandal'); 'ok'");
    CHECK(fx.evalString("s.describe()") == "Durandal (10)");
    CHECK(fx.evalBool("s instanceof forge.Sword"));
}

TEST_CASE("field props read/write; readonly rejects writes") {
    ForgeFixture fx;
    fx.evalString("globalThis.s = new forge.Sword('Edge', 3); 'ok'");
    CHECK(fx.evalString("s.name") == "Edge");
    CHECK(fx.evalString("s.name = 'Blunt'; s.name") == "Blunt");
    CHECK(fx.evalInt("s.damage") == 3);
    CHECK(fx.evalBool("(() => { 'use strict'; try { s.damage = 99; return false; }"
                      " catch (e) { return e instanceof TypeError; } })()"));
    CHECK(fx.evalInt("s.damage") == 3);
    // wrong type into a typed field
    CHECK(fx.evalString("try { s.name = 42 } catch (e) { e.message }") ==
          "Sword.name: expected string, got number");
}

TEST_CASE("getter/setter pair props") {
    ForgeFixture fx;
    fx.evalString("globalThis.s = new forge.Sword('Edge', 3); 'ok'");
    CHECK(fx.evalInt("s.secret") == 0);
    CHECK(fx.evalInt("s.secret = 21; s.secret") == 42);  // setter doubles
}

TEST_CASE("class shape matches a JS class") {
    ForgeFixture fx;
    fx.evalString("globalThis.s = new forge.Sword('Edge', 3); 'ok'");
    CHECK(fx.evalBool("Object.getPrototypeOf(s) === forge.Sword.prototype"));
    CHECK(fx.evalBool("Object.getPrototypeOf(s).hasOwnProperty('strike')"));
    CHECK(fx.evalBool("!s.hasOwnProperty('strike')"));
    CHECK(fx.evalBool(
        "typeof Object.getOwnPropertyDescriptor(forge.Sword.prototype, 'name').get === "
        "'function'"));
    // methods and accessors are non-enumerable, like real JS classes
    CHECK(fx.evalInt("Object.keys(forge.Sword.prototype).length") == 0);
    CHECK(fx.evalString("forge.Sword.name") == "Sword");
}

TEST_CASE("static methods live on the constructor") {
    ForgeFixture fx;
    const int before = fx.evalInt("forge.Sword.forged()");
    fx.evalString("new forge.Sword('a', 1); 'ok'");
    CHECK(fx.evalInt("forge.Sword.forged()") == before + 1);
    CHECK(fx.evalBool("!forge.Sword.prototype.hasOwnProperty('forged')"));
}

TEST_CASE("constructor misuse errors") {
    ForgeFixture fx;
    CHECK(fx.evalString("try { forge.Sword('x', 1) } catch (e) { e.message }") ==
          "Class constructor Sword cannot be invoked without 'new'");
    CHECK(fx.evalString("try { new forge.Sword(42, 1) } catch (e) { e.message }") ==
          "arg 1 (name) of new Sword: expected string, got number");
    CHECK(fx.evalString("try { new forge.Sword('x') } catch (e) { e.message }") ==
          "arg 2 (damage) of new Sword: expected number, got undefined");
}

TEST_CASE("constructor and method overload sets") {
    ForgeFixture fx;
    CHECK(fx.evalString("new forge.Potion().sip()") == "unnamed:1");
    CHECK(fx.evalString("new forge.Potion('mana', 3).sip()") == "mana:1");
    CHECK(fx.evalString("new forge.Potion('mana', 3).sip(2)") == "mana:2");
    CHECK(fx.evalInt("new forge.Potion('mana', 3).doses") == 3);
    CHECK(fx.evalString("try { new forge.Potion(42) } catch (e) { e.message }") ==
          "no constructor overload of Potion matches the given arguments (number)");
    CHECK(fx.evalString("try { new forge.Potion().sip('x') } catch (e) { e.message }") ==
          "no overload of Potion.sip matches the given arguments (string)");
}

TEST_CASE("class-level constants sit on the constructor") {
    ForgeFixture fx;
    CHECK(fx.evalInt("forge.Sword.MAX_DAMAGE") == 9000);
    CHECK(fx.evalBool("(() => { 'use strict'; try { forge.Sword.MAX_DAMAGE = 1; return false; }"
                      " catch { return true; } })()"));
    // it lives on the constructor, not on instances or the prototype
    CHECK(fx.evalBool("!('MAX_DAMAGE' in new forge.Sword('s', 1))"));
    CHECK(fx.evalBool("!Object.hasOwn(forge.Sword.prototype, 'MAX_DAMAGE')"));
}

TEST_CASE("methods reject foreign receivers") {
    ForgeFixture fx;
    CHECK(fx.evalBool("(() => { try { forge.Sword.prototype.strike.call({}, 1); return false; }"
                      " catch (e) { return e instanceof TypeError; } })()"));
    CHECK(fx.evalBool(
        "(() => { const d = Object.getOwnPropertyDescriptor(forge.Sword.prototype, 'name');"
        " try { d.get.call({}); return false; } catch (e) { return e instanceof TypeError; } })()"));
}
