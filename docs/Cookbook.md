# Alka cookbook

Focused recipes. The [Tutorial](Tutorial.md) introduces the basics; the
[Reference](Reference.md) lists every option.

- [Naming conventions](#naming-conventions)
- [JS subclasses that override C++ virtuals](#js-subclasses-that-override-c-virtuals)
- [Objects C++ owns and may destroy](#objects-c-owns-and-may-destroy)
- [Containers: iteration, indexing, live views](#containers-iteration-indexing-live-views)
- [Value types as plain JS objects](#value-types-as-plain-js-objects)
- [Enums](#enums)
- [JS callbacks stored in C++ objects](#js-callbacks-stored-in-c-objects)
- [Calling JS from other threads](#calling-js-from-other-threads)
- [Long-running work and cancellation](#long-running-work-and-cancellation)
- [Binary data without copies](#binary-data-without-copies)
- [Resources with an explicit end: `using` and `dispose()`](#resources-with-an-explicit-end-using-and-dispose)
- [Garbage-collected C++ objects (cppgc)](#garbage-collected-c-objects-cppgc)
- [Running untrusted scripts](#running-untrusted-scripts)
- [Dynamic properties and callable objects](#dynamic-properties-and-callable-objects)
- [64-bit integers](#64-bit-integers)
- [Debugging scripts in Chrome DevTools](#debugging-scripts-in-chrome-devtools)
- [Startup snapshots](#startup-snapshots)
- [A Node.js addon](#a-nodejs-addon)
- [An Electron addon](#an-electron-addon)
- [Packaging: find_package and vcpkg](#packaging-find_package-and-vcpkg)

## Naming conventions

Reflected names are used as written. A binding's `naming` turns them into JS
names, step by step:

```cpp
struct Unit { int hit_points_ = 10; int get_level() const; };
template<> struct Alka::Bind<Unit> : Alka::Class<Unit> {
    static constexpr auto naming = Alka::naming(Alka::stripSuffix<"_">, Alka::camelCase);
    static constexpr auto members = Alka::members(
        Alka::prop<&Unit::hit_points_>(), Alka::method<&Unit::get_level>());   // hitPoints, getLevel()
};
```

| C++ | naming |
|---|---|
| `mHp`, `m_hp`, `sCount` | `Alka::memberPrefixes` |
| `m_max_speed` (Qt, MFC) | `Alka::naming(Alka::stripPrefix<"m_">, Alka::camelCase)` |
| `hit_points_`, `get_level()` (Google, Chromium) | `Alka::naming(Alka::stripSuffix<"_">, Alka::camelCase)` |
| `bIsAlive`, `GetHealth()`, `FVector` (Unreal) | `Alka::naming(Alka::only<Alka::NameKind::Property>(Alka::stripPrefix<"b">), Alka::only<Alka::NameKind::Type>(Alka::stripPrefix<"A", "U", "F">), Alka::camelCase)` |
| `player_state` (a type) | `Alka::pascalCase` |

- Steps run in order. `camelCase` works on methods, properties and functions,
  `pascalCase` on type names, the others on every kind of name;
  `only<NameKind...>(step)` sets the kinds exactly -- enumerators, say:
  `Alka::only<Alka::NameKind::EnumValue>(Alka::camelCase)`. A naming is a step
  too: `Alka::naming(Alka::memberPrefixes, Alka::camelCase)`.
- A name no rule fits: `Alka::rename<"mOSes", "osList">` -- exactly this C++
  name becomes exactly this JS name, whatever the other steps would do. Or name
  that declaration: `Alka::prop<"osList", &Unit::mOSes>()`.
- A rule of your own is any constexpr callable taking `(Alka::NameKind,
  std::string_view)` or `(std::string_view)` and returning a string
  (`std::string_view`, a literal, or an `Alka::NameText` you build; `std::string`
  too, except with clang on libstdc++):

  ```cpp
  Alka::naming([](Alka::NameKind kind, std::string_view name) {
      return kind == Alka::NameKind::EnumValue && name.starts_with('k') ? name.substr(1) : name;
  })
  ```

- A class without a naming takes its bound base's. For a whole project:
  `target_compile_definitions(Alka PUBLIC ALKA_NAMING=Alka::memberPrefixes)` (a
  naming of your own: define it in a header named by `ALKA_NAMING_HEADER`), or
  derive your bindings from a base that declares one:
  `template<class T> struct GameClass : Alka::Class<T> { static constexpr auto naming = ...; };`.
- A module's naming names its functions, enums and enumerators; a record's, its
  fields (both ways); a class's, its members and the class itself.
- Two members ending up with one JS name are a compile error that names it, and
  so are names V8 uses itself (`constructor` on instances; `prototype`, `name`,
  `length`, `arguments`, `caller` on a class).

## JS subclasses that override C++ virtuals

A trampoline class forwards each virtual to a JS override when there is one:

```cpp
class Animal {
public:
    virtual ~Animal() = default;
    virtual std::string speak() const { return "..."; }
    virtual int legs() const = 0;
};

struct JsAnimal : Animal, Alka::Overridable<Animal> {
    std::string speak() const override { return ALKA_OVERRIDE(std::string, Animal, speak); }
    int legs() const override { return ALKA_OVERRIDE_PURE(int, Animal, legs); }
};

template<> struct Alka::Bind<Animal> : Alka::Class<Animal> {
    using Trampoline = JsAnimal;   // JS constructs the trampoline
    static constexpr auto members = Alka::members(
        Alka::ctor<>(),
        Alka::method<"speak", &Animal::speak>(),   // virtuals need explicit names under MSVC
        Alka::method<"legs", &Animal::legs>());
};
```

```js
class Dog extends zoo.Animal {
    speak() { return 'woof (' + super.speak() + ')'; }   // super reaches the C++ base
    legs() { return 4; }
}
zoo.adopt(new Dog());   // C++ calling speak() now runs the JS method
```

- A JS subclass object handed to C++ as `std::shared_ptr<Animal>` is pinned: its
  wrapper (overrides, expandos) lives as long as C++ holds it.
- `ALKA_OVERRIDE_PURE` throws "is abstract" when JS does not override a pure virtual.
- `ALKA_OVERRIDE` takes the C++ method name and looks the override up under what
  the class's naming makes of it (`Speak` under `camelCase`: `speak`). When the
  method is bound under another name: `Alka::overrideOr<R, "jsName">(this,
  fallback, args...)`.

## Objects C++ owns and may destroy

A `T*` / `T&` result lends JS an object C++ keeps owning. Under strict borrowing
(`ALKA_STRICT_BORROW`, on by default) the class or declaration must say why that
is safe:

```cpp
// 1. Tracked: destruction (on any thread) detaches every wrapper
class Enemy : public Alka::Trackable { ... };
Enemy* nearestEnemy();                 // JS keeps a handle; after C++ deletes the enemy,
                                       // using it throws "... not (or no longer) alive"

// 2. Declared lifetime
template<> struct Alka::Bind<World> : Alka::Class<World> {
    static constexpr auto lifetime = Alka::Lifetime::Static;   // outlives the isolate
    // Lifetime::Manual: you call Alka::invalidate(ptr) before destroying it
    ...
};

// 3. This declaration vouches for it
Alka::method<&Squad::leader>(Alka::policy::borrow);
```

Other policies on a declaration:

| Policy | Result |
|---|---|
| `policy::copy` | JS gets its own copy |
| `policy::move` | moved out of a reference into a JS-owned object |
| `policy::take` | JS takes ownership of a returned pointer (deletes it) |
| `policy::internalRef` | a view of a part of `this`; keeps `this` alive |
| `keepAlive<I>` | `this` keeps argument `I` alive (`releaseKept` lets go) |

`std::shared_ptr<T>` results are co-owned; `std::weak_ptr<T>` results stop working
when C++ lets go; `enable_shared_from_this` objects are shared automatically.
C++ cannot take *sole* ownership of what JS made (a `std::unique_ptr<T>`
parameter is refused at compile time): take `std::shared_ptr<T>`.

## Containers: iteration, indexing, live views

```cpp
struct Polygon {
    std::vector<Point> mPoints;
    std::size_t size() const { return mPoints.size(); }
    Point& operator[](std::size_t i) { return mPoints[i]; }
    void clear() { mPoints.clear(); }
};

template<> struct Alka::Bind<Polygon> : Alka::Class<Polygon> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&Polygon::clear>(),
        Alka::indexer(Alka::policy::internalRef),                 // polygon[i]
        Alka::iterable<Polygon>(Alka::policy::internalRef));      // for...of, spread
};
```

```js
const p = polygon[0];   // a live view of element 0
polygon.add(5);         // the vector may reallocate: p still finds element 0
polygon.clear();        // now p throws "... is not (or no longer) alive (nothing is there now)"
```

- Without `internalRef`, elements are copied (copyable types) -- no view at all.
- Views keep their owner alive; the same path gives the same view
  (`polygon[0] === polygon[0]`).
- Fields of class type (`Alka::prop<&Line::mFrom>()`) are views the same way.
- `std::vector<T>` / `std::map` / `std::set` / `std::optional` parameters and
  results convert to arrays, objects or `Map`, sets, `undefined`.

## Value types as plain JS objects

```cpp
struct Color { double r = 0, g = 0, b = 0; };
template<> struct Alka::Bind<Color> : Alka::Record<Color> {
    static constexpr auto members = Alka::members(
        Alka::prop<&Color::r>(), Alka::prop<&Color::g>(), Alka::prop<&Color::b>());
};
Color brighten(Color c);    // JS: brighten({ r: 1, g: 0.5, b: 0 }) -> { r: ..., g: ..., b: ... }
```

Records convert by value (no wrapper, no identity) and appear as `interface`s in
the `.d.ts`; missing optional fields keep their defaults, wrong ones report
`key 'g'`.

## Enums

```cpp
enum class Weather { Sunny, Rain, Snow };
// module member:
Alka::enumType<Weather>()        // JS: game.Weather.Rain === 1, game.Weather[1] === 'Rain'
```

Enum parameters accept the numeric values; names come from compile-time
reflection over `ALKA_ENUM_RANGE_MIN..MAX` (or `Alka::EnumRange<E>`).

## JS callbacks stored in C++ objects

A `Function` handle held by a C++ object that its own wrapper owns would be a
cycle the GC cannot see (closure -> wrapper -> C++ -> closure). `Retained`
stores the value on the owner's wrapper instead:

```cpp
struct Button {
    Alka::Retained<Alka::Function<void()>> mOnClick;
    void click() { if (auto f = mOnClick.get(); f.valid()) f.call(); }
};
template<> struct Alka::Bind<Button> : Alka::Class<Button> {
    static constexpr auto naming = Alka::memberPrefixes;
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&Button::click>(), Alka::prop<&Button::mOnClick>());
};
```

```js
const b = new ui.Button();
b.onClick = () => b.click();   // collectable once nothing else holds b
```

From C++: `mOnClick.set(this, fn)`. A plain `Alka::Function<R(Args...)>` is fine
when the C++ side does not belong to the JS object (a global registry, a
one-shot call).

## Calling JS from other threads

```cpp
void watch(Alka::ThreadSafeFunction<void(int)> onChange) {
    std::thread([onChange] {
        for (int i = 0; i < 10; ++i) onChange.post(i);   // queued, in order, run on the isolate thread
    }).detach();
}
```

`call()` returns a future of the result. Pending calls do not keep the host
loop running unless `keepAlive(true)` says so (like a referenced N-API
thread-safe function).
For one result, `Alka::Promise<T>::create()` and `promise.settler()` settle from
any thread:

```cpp
Alka::Promise<int> compute(int x) {
    auto promise = Alka::Promise<int>::create();
    std::thread([s = promise.settler(), x] { s.resolve(slow(x)); }).detach();
    return promise;   // a pending settler keeps the host loop alive
}
```

## Long-running work and cancellation

```cpp
Alka::Task<std::string> download(std::string url, Alka::AbortSignal signal) {
    while (!done) {
        signal.throwIfAborted();                          // rejects with the JS reason
        co_await Alka::offload([&] { fetchChunk(url); });  // on a worker thread
    }
    co_return result;
}
```

```js
const controller = new AbortController();
const page = net.download(url, controller.signal);
setTimeout(() => controller.abort(), 5000);
```

Coroutine parameters must own their data (references and views are refused at
compile time); a coroutine method keeps its receiver alive until it settles.
`co_await Alka::await<T>(jsPromise)` waits for a JS promise;
`co_await Alka::resumeOn(dispatcher)` returns to the isolate thread after a
foreign awaitable resumed elsewhere.

## Binary data without copies

```cpp
// in: any typed array / ArrayBuffer, pinned for the call (writable span: writes through)
double sum(std::span<const double> values);
void normalize(std::span<float> values);

// in, kept: BufferView keeps the memory alive -- safe to hand to another thread
void upload(Alka::BufferView bytes);

// out: C++ fills memory JS then sees as a Float32Array -- no copy
Alka::TypedView<float> mesh(Alka::CallInfo call, int n) {
    auto out = Alka::TypedView<float>::allocate(call.mIsolate, 3 * n);
    fill(out);
    return out;
}
```

Shared and resizable buffers are refused unless asked for
(`SharedBufferView`, `SharedTypedView<T>`: their contents may change under C++).
`std::vector<std::uint8_t>` and `std::vector<std::byte>` convert to and from
`Uint8Array` with a copy.

## Resources with an explicit end: `using` and `dispose()`

```cpp
struct File { void close(); std::string read(); };
template<> struct Alka::Bind<File> : Alka::Class<File> {
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string>(), Alka::method<&File::read>(),
        Alka::disposable<&File::close>());   // file.dispose() and [Symbol.dispose]
};
```

```js
{
    using file = new io.File('a.txt');   // closed at the end of the block
    file.read();
}
```

After `dispose()` the wrapper lets go of the object (destroyed once no native
frame still uses it); further calls throw instead of touching it.

## Garbage-collected C++ objects (cppgc)

When C++ objects and JS objects reference each other freely (scene graphs,
DOM-like trees), let V8's C++ heap own them -- one garbage collector for both
sides, cycles included:

```cpp
class Node : public Alka::GcObject<Node> {   // GcObject must be the first base
public:
    void Trace(cppgc::Visitor* visitor) const override {
        GcObject::Trace(visitor);   // its wrapper
        visitor->Trace(mParent);
        for (const auto& child : mChildren) visitor->Trace(child);
    }
    cppgc::Member<Node> mParent;
    std::vector<cppgc::Member<Node>> mChildren;
};

Node* makeNode() { return Alka::makeGc<Node>(); }
cppgc::Persistent<Node> gRoot;   // a root C++ keeps
```

- Needs a C++ heap: `Engine::createIsolate({.mCppHeap = true})`, a
  `CreateParams::cpp_heap` of your own, or Node's (Node 24+). V8 13 or newer.
- Such objects travel as `T*`, `T&`, `cppgc::Member`, `WeakMember`,
  `Persistent` or `WeakPersistent`; copies, `shared_ptr`, `dispose` and
  trampolines are refused.
- Each has exactly one JS wrapper; JS expandos on it live as long as the object.

## Running untrusted scripts

```cpp
auto isolate = Alka::Engine::createIsolate({
    .mMaxHeapBytes = std::size_t{64} << 20,                 // stopped, not crashed, at the limit
    .mOnError = [](std::string_view message) { log(message); }});
Alka::ContextScope scope(isolate.get(), {
    .mConsoleSink = [](Alka::ConsoleLevel, std::string_view text) { log(text); },
    .mModuleRoot = "sandbox/"});                             // where relative imports resolve
scope.install<SafeApi>();                                    // only what scripts may use
auto result = scope.runModuleFile("sandbox/main.mjs", {.mTimeout = std::chrono::seconds(2)});
// "the script timed out after 2000 ms" / "JS heap limit reached (64 MB)"
```

The isolate stays usable after either limit. `Alka::Watchdog` gives the same
time limit around any code of your own; `ContextOptions` turns off timers,
console or the web globals.

## Dynamic properties and callable objects

```cpp
struct Config {
    std::map<std::string, std::string> mValues;
    std::optional<std::string> get(const std::string& key) const;   // empty: no such property
    void set(const std::string& key, std::string value);
    std::vector<std::string> keys() const;
    std::string lookup(const std::string& key) const;
};
template<> struct Alka::Bind<Config> : Alka::Class<Config> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(),
        Alka::namedProperties<&Config::get, &Config::set, nullptr, nullptr, &Config::keys>(),  // config.anyKey
        Alka::asFunction<&Config::lookup>());                                                 // config('key')
};
```

Named properties never shadow the class's own members; with a key lister they
are enumerable (`Object.keys(config)`). Static members: `Alka::staticMethod`,
`Alka::staticProp<&Game::sCount>()` (`Game.sCount`, or `Game.count` under
`memberPrefixes`; inherited by subclass constructors).

## 64-bit integers

`std::int64_t` / `std::uint64_t` results are `bigint` by default (exact).
Choose per declaration or globally:

```cpp
Alka::fn<&fileSize>(Alka::int64As<Alka::Int64Mode::Number>)   // number (exact up to 2^53)
// or compile with ALKA_INT64_MODE=Auto: number when exact, bigint beyond
```

Parameters accept both `number` and `bigint`; a `number` past 2^53 is refused
rather than rounded.

## Debugging scripts in Chrome DevTools

```cpp
#include <Alka/Engine/Inspector.hpp>
#include <InspectorBridge/InspectorBridge.hpp>   // tools/InspectorBridge (target Alka::InspectorBridge)

Alka::Inspector inspector(isolate.get());
inspector.contextCreated(scope.context(), "game");
AlkaTools::InspectorBridge bridge(inspector, isolate.get(), 9229, "game");
// open chrome://inspect, or bridge.frontendUrl()
bridge.waitForFrontend();
bridge.pauseOnStart();          // stop at the first line
scope.runModuleFile("main.mjs");
```

`DemoHost --inspect-brk script.js` does exactly this. The bridge listens on
loopback only; console output reaches DevTools as well as the console sink.

## Startup snapshots

Bake bindings and setup scripts into a snapshot to start isolates faster:

```cpp
// build time
v8::Isolate::CreateParams params;
params.external_references = Alka::snapshot::externalReferences<Game>();
v8::SnapshotCreator creator(params);
std::vector<std::size_t> templates;
{   // enter the isolate and a context, Alka::install<Game>(context), run setup scripts
    templates = Alka::snapshot::addTemplates<Game>(creator);
    creator.SetDefaultContext(context, Alka::snapshot::internalFieldsSerializer());
}
Alka::snapshot::prepare(creator.GetIsolate());
auto blob = creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);

// start-up
params.snapshot_blob = &blob;
auto* isolate = v8::Isolate::New(params);
Alka::snapshot::restoreTemplates<Game>(isolate, templates);
auto context = v8::Context::New(isolate, nullptr, {}, {}, Alka::snapshot::internalFieldsDeserializer());
```

Native objects do not travel: wrappers in the snapshot come back detached.

## A Node.js addon

```cpp
#include <node.h>
#include <Alka/Alka.hpp>
#include <Alka/Host/Node.hpp>

NODE_MODULE_INIT(/* exports, module, context */) {
    Alka::Node::attach(context);                      // event loop, keep-alive, teardown
    Alka::installInto<Api>(context, exports);
}
```

```cmake
add_library(NodeV8 INTERFACE)
target_include_directories(NodeV8 SYSTEM INTERFACE "${NODE_DIR}/include/node")
target_compile_definitions(NodeV8 INTERFACE BUILDING_NODE_EXTENSION USING_V8_SHARED=1 USING_UV_SHARED=1)
if(WIN32)
  target_link_libraries(NodeV8 INTERFACE "${NODE_DIR}/node.lib")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(NodeV8 INTERFACE COMPONENT_BUILD)   # Node is built with GCC
endif()
set(ALKA_V8_TARGET NodeV8)
add_subdirectory(Alka)
add_library(game MODULE addon.cpp)
set_target_properties(game PROPERTIES PREFIX "" SUFFIX ".node")
target_link_libraries(game PRIVATE Alka::Alka)
```

- Promises settled from other threads, offloaded coroutines and referenced
  thread-safe functions keep the process alive until they finish.
- Worker threads get their own isolates; Alka lets go of each when its
  environment ends.
- Node's release headers omit `v8-fast-api-calls.h`; add it (matching Node's V8
  version, see `ci/fetch-v8-headers.sh`) to get Fast API paths.

## An Electron addon

The same addon code runs in Electron. The build is what differs, because Alka
uses V8's C++ API. That API passes `std` types, and Electron's are those of
its own libc++ (namespace `std::__Cr`), not the system's.
`ci/electron-addon.sh <version> <dir>` does all of the following for Linux x64.
It downloads the release, builds `tests/Node` against it, and runs the tests
inside Electron:

- **Headers and V8 ABI.** Use the release's node headers
  (`artifacts.electronjs.org/headers/dist/v<version>/`). Define the V8 macros
  its `config.gypi` implies. For Electron 45 they are:
  - `V8_COMPRESS_POINTERS`
  - `V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES`
  - `V8_31BIT_SMIS_ON_64BIT_ARCH`
  - `V8_ENABLE_SANDBOX`
- **Electron's libc++.** Compile with `-nostdinc++` against the release's
  `libcxx_headers.zip` and `libcxxabi_headers.zip`. Also pass
  `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE` and
  `-D_LIBCPP_INSTRUMENTED_WITH_ASAN=0`, which Chromium's build passes besides
  `__config_site`. Link `libcxx-objects-v<version>-linux-x64.zip` with
  `-nostdlib++`.
- **Chromium's clang.** Those libc++ objects are ThinLTO bitcode of Chromium's
  clang, so compile and link with that clang and its lld. Chromium's
  `tools/clang/scripts/update.py`, at the tag of the release's
  `process.versions.chrome`, names the package.
- **No C++-heap classes.** Define `ALKA_HAS_CPPGC=0`: Electron exports no
  cppgc allocation internals.

Verified with Electron 45 (V8 15.6) on Linux. Electron on Windows and macOS
has not been tried.

## Packaging: find_package and vcpkg

An installed Alka is a *source* package -- headers, sources and
`AlkaConfig.cmake` -- compiled inside your project against the V8 you name:

```sh
cmake -S Alka -B build -DALKA_PACKAGE_ONLY=ON && cmake --install build --prefix /opt/alka
# or: vcpkg install alka --overlay-ports=Alka/ports
```

```cmake
set(ALKA_V8_TARGET MyV8)
find_package(Alka 0.2 CONFIG REQUIRED)
target_link_libraries(app PRIVATE Alka::Alka)
```
