# Alka tutorial

This walk-through builds a small scripting host: C++ types exposed to JavaScript,
scripts that use them, the `.d.ts` editors need, and the same bindings inside a
Node.js addon. Each step builds on the previous one; the
[Cookbook](Cookbook.md) has focused recipes and the [Reference](Reference.md)
every name.

## 1. What you need

- A C++23 compiler: MSVC 19.5x (Visual Studio 2026), clang 20+, or GCC 15.
- CMake 3.28+.
- V8, as a CMake target. Alka never downloads or builds V8 ("bring your own
  V8"): you name a target that carries V8's headers, the ABI definitions V8 was
  built with, and its libraries.

```cmake
# Your V8 -- e.g. the v8-v143-x64 NuGet package on Windows (see ci/NugetV8.cmake),
# libnode-dev on Linux (ci/LibnodeV8.cmake), or a V8 you built.
add_library(MyV8 INTERFACE IMPORTED)
set_target_properties(MyV8 PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${V8_ROOT}/include"
  # exactly what V8 was built with -- object layouts depend on it
  INTERFACE_COMPILE_DEFINITIONS "V8_COMPRESS_POINTERS;V8_31BIT_SMIS_ON_64BIT_ARCH;V8_ENABLE_SANDBOX"
  INTERFACE_LINK_LIBRARIES "${V8_ROOT}/lib/v8.dll.lib;${V8_ROOT}/lib/v8_libbase.dll.lib;${V8_ROOT}/lib/v8_libplatform.dll.lib")
```

Then bring Alka in, either from an installed package (it is compiled in your
project, against your V8):

```cmake
set(ALKA_V8_TARGET MyV8)
set(ALKA_WITH_ENGINE ON)          # the self-hosting layer used below (needs libplatform)
find_package(Alka 0.2 CONFIG REQUIRED)
```

or from source (`add_subdirectory`, `FetchContent`):

```cmake
set(ALKA_V8_TARGET MyV8)
set(ALKA_BUILD_ENGINE ON)
add_subdirectory(third_party/Alka)
```

and link it: `target_link_libraries(game PRIVATE Alka::Alka)`.

## 2. Hello, script

The Engine layer starts V8, makes isolates, and gives contexts what scripts
expect -- `console`, timers, `TextEncoder`, `AbortController`, module files:

```cpp
#include <Alka/Alka.hpp>
#include <Alka/Engine/Engine.hpp>

int main() {
    auto isolate = Alka::Engine::createIsolate();   // starts V8 on first use
    Alka::ContextScope scope(isolate.get());

    auto result = scope.eval(R"(
        setTimeout(() => console.log('later'), 10);
        console.log('hello', { from: 'JS' });
        6 * 7
    )");
    if (!result) {
        std::printf("error: %s\n", result.error().message().c_str());
        return 1;
    }
    // run timers, promises and posted work until nothing is left
    return Alka::Engine::runUntilIdle(isolate.get()) ? 0 : 1;
}
```

```text
hello { from: 'JS' }
later
```

`eval` returns `Alka::Result<Alka::Value>` -- a `std::expected` whose error is
the JS exception's message (and the exception itself, see
`Alka::exceptionValue`). `runUntilIdle` returns `false` when an error nobody
handled was reported meanwhile (an unhandled rejection, a throwing timer).

## 3. Binding functions

Bindings are specializations of `Alka::Bind`. A module groups functions,
classes, constants and enums under a JS namespace:

```cpp
namespace game {
int roll(int sides) { return 1 + std::rand() % sides; }
std::string greet(std::string_view name) { return "Hello, " + std::string(name); }
struct Api;  // a tag naming the module
}

template<>
struct Alka::Bind<game::Api> : Alka::Module<"game"> {
    static constexpr auto members = Alka::members(
        Alka::fn<&game::roll>(Alka::arg("sides") = 6),     // name reflected: "roll"
        Alka::fn<&game::greet>(Alka::arg("name")),
        Alka::var<"version">(2));
};
```

```cpp
scope.install<game::Api>();
scope.eval("console.log(game.greet('Ada'), game.roll(), game.roll(20), game.version)");
```

- Names come from the C++ declaration (`roll`); give one explicitly with
  `fn<"name", &f>()`.
- Arguments are checked like a typed language would: `game.roll('x')` throws
  `TypeError: arg 1 (sides) of game.roll: expected number, got string`, and
  `game.roll(1.5)` refuses the fraction instead of truncating it.
- `arg("sides") = 6` declares a default; JS may then leave the argument out.

## 4. Binding a class

```cpp
namespace game {
class Unit {
public:
    Unit(std::string name, int hp) : mName(std::move(name)), mHp(hp) {}
    void hit(int damage) { mHp = std::max(0, mHp - damage); }
    [[nodiscard]] bool alive() const { return mHp > 0; }
    std::string mName;
    int mHp = 0;
};
}

template<>
struct Alka::Bind<game::Unit> : Alka::Class<game::Unit> {
    static constexpr auto doc = "Something on the battlefield.";
    static constexpr auto naming = Alka::memberPrefixes;    // mName -> "name", mHp -> "hp"
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("hp") = 100),
        Alka::method<&game::Unit::hit>(Alka::arg("damage")),
        Alka::method<&game::Unit::alive>(),
        Alka::prop<&game::Unit::mName>(Alka::readonly),
        Alka::prop<&game::Unit::mHp>());
};

// and in the module: Alka::type<game::Unit>
```

Names come from the declarations as written; a binding's `naming` turns them
into the JS ones. `Alka::memberPrefixes` drops `m` / `m_` from properties
(`s` / `s_` from static ones); other codebases compose their own, e.g.
`Alka::naming(Alka::stripSuffix<"_">, Alka::camelCase)` for `hit_points_` →
`hitPoints` ([Cookbook](Cookbook.md#naming-conventions)).

```js
const orc = new game.Unit('orc', 30);
orc.hit(12);
console.log(orc.name, orc.hp, orc.alive());   // orc 18 true
orc.name = 'elf';                              // read-only: ignored (a TypeError in strict code)
class Boss extends game.Unit {                 // JS subclasses work
    constructor() { super('boss', 500); }
}
```

- The object JS created belongs to JS: it is destroyed when the wrapper is
  collected (or when its isolate is disposed).
- The same C++ object always appears as the same JS object (per context), so
  `===` and expandos behave.
- Inheritance mirrors C++: declare `using Extends = Base;` in the derived
  binding, and `instanceof`, method lookup and down-casts follow.

## 5. Types for editors

The bindings are also the source of a TypeScript declaration file:

```cpp
#include <Alka/Ts/Generator.hpp>
std::string dts = Alka::generateDts<game::Api>();                      // declare namespace game {...}
Alka::TsGenOptions esm;
esm.mStyle = Alka::TsStyle::EsModule;
std::string module = Alka::generateDts<game::Api>(esm);                 // declare module "game" {...}
```

```ts
declare namespace game {
    /** Something on the battlefield. */
    class Unit {
        constructor(name: string, hp?: number);
        hit(damage: number): void;
        alive(): boolean;
        readonly name: string;
        hp: number;
    }
    function roll(sides?: number): number;
    function greet(name: string): string;
    const version: number;
}
```

## 6. Who owns what

Whenever C++ hands an object to JS, Alka must know whether JS may keep it. The
result type says so:

| C++ returns | JS gets |
|---|---|
| `T` (by value), `std::unique_ptr<T>` | its own object (JS owns it) |
| `std::shared_ptr<T>` | a share of it (C++ and JS co-own) |
| `std::weak_ptr<T>` | a view usable while C++ keeps the object |
| `T*`, `T&` | a borrowed view -- see below |

A borrowed view of an object C++ may destroy would be a dangling pointer in
waiting, so Alka refuses it at compile time unless the class says how its
lifetime works:

```cpp
// C++ destroys units whenever it likes: wrappers learn of it and refuse use
class Unit : public Alka::Trackable { ... };

// or: this object outlives the isolate
template<> struct Alka::Bind<World> : Alka::Class<World> {
    static constexpr auto lifetime = Alka::Lifetime::Static;
    ...
};
```

or the declaration takes responsibility: `Alka::method<&Squad::leader>(Alka::policy::borrow)`.
A destroyed `Trackable` object's wrapper throws `the native Unit object is not
(or no longer) alive` instead of touching freed memory.

Views of parts -- `squad.members[2]`, a field of class type -- stay valid while
the container changes: Alka finds the part again on each use (and reports when
it is gone), and the view keeps its owner alive.

## 7. Errors

C++ exceptions thrown from bound code become JS errors with the message:

```cpp
void move(int x) { if (x < 0) throw std::out_of_range("off the map"); }
```

```js
try { unit.move(-1); } catch (e) { console.log(e.message); }   // off the map
```

Exception types can become JS error classes of their own -- a module member
exports the class, and an optional decorator copies fields onto each error:

```cpp
struct NotFound : std::runtime_error { using runtime_error::runtime_error; int mId = 0; };

// in Bind<game::Api>::members:
//     Alka::error<"NotFound", game::NotFound>()
// extra fields (register before installing):
Alka::registerError<game::NotFound>("NotFound", [](const game::NotFound& e, Alka::Object& error) {
    (void)error.set("id", e.mId);
});
```

```js
try { game.find(7); } catch (e) { e instanceof game.NotFound; e.id; }
```

A function returning `std::expected<T, E>` returns `T` to JS or throws `E`.

## 8. Calling back into JS, and async work

```cpp
// a JS function as a C++ value
int applyTwice(const Alka::Function<int(int)>& f, int x) { return f.call(f.call(x)); }

// a coroutine: JS gets a Promise
Alka::Task<std::string> loadMap(std::string name) {
    std::string text = co_await Alka::offload([name] { return readFile(name); });  // a worker thread
    co_return parse(text);                                                       // back on the isolate thread
}
```

```js
game.applyTwice((x) => x * 3, 2);          // 18
const map = await game.loadMap('forest');
```

`Alka::Promise<T>` and its `Settler<T>` settle promises from any thread;
`Alka::ThreadSafeFunction<void(T)>` calls JS from other threads in order.

## 9. Scripts as module files

```js
// main.mjs
import { Unit } from 'game';          // a bound module (see registerModules)
import { arena } from './arena.mjs';  // a file next to this one
import config from './config.json' with { type: 'json' };

const hero = new Unit('hero', config.hp);
arena.add(hero);
console.log(import.meta.url);
await arena.fight();
```

```cpp
scope.registerModules<game::Api>();
auto result = scope.runModuleFile("scripts/main.mjs", {.mTimeout = std::chrono::seconds(10)});
Alka::Engine::runUntilIdle(isolate.get());
```

Untrusted scripts get limits: `EvalOptions::mTimeout` terminates a runaway
script, and `IsolateOptions::mMaxHeapBytes` stops one that allocates too much --
both leave the isolate usable.

## 10. Your own host

The Engine layer is optional. A host that already has isolates (a game engine,
a browser-like shell) installs bindings into any context:

```cpp
Alka::install<game::Api>(context);       // into the context's global object

Alka::Dispatcher dispatcher(isolate);    // work other threads post (Settler results,
dispatcher.setWakeup([&] { loop.wake(); });  // offloaded coroutines): wake your loop,
dispatcher.drain();                      // and drain on the isolate thread

Alka::disposeIsolate(isolate);           // before isolate->Dispose()
```

## 11. Inside Node.js

The same bindings work in a Node addon (and Electron), on Node's own V8:

```cpp
#include <node.h>
#include <Alka/Alka.hpp>
#include <Alka/Host/Node.hpp>

NODE_MODULE_INIT(/* exports, module, context */) {
    Alka::Node::attach(context);                       // Node's event loop runs Alka's work
    Alka::installInto<game::Api>(context, exports);    // module.exports = { roll, Unit, ... }
}
```

```js
const game = require('./build/game.node');
const unit = new game.Unit('orc', 30);
```

Build every file -- Alka's too -- with what `node.h` defines for addons
(`BUILDING_NODE_EXTENSION`, `USING_V8_SHARED`, `USING_UV_SHARED`); see
`tests/Node/CMakeLists.txt` for a complete build. An Electron addon is the
same code built with Electron's toolchain and libc++: see
[An Electron addon](Cookbook.md#an-electron-addon).

## Next

- [Cookbook](Cookbook.md): overrides from JS, containers, binary data, records,
  garbage-collected C++ objects, sandboxing, debugging, snapshots.
- [Reference](Reference.md): every declaration and option.
- [Performance](Performance.md): what a call costs, and how to make it cheaper.
