# Alka

A declarative, memory-safe C++23 ↔ V8 binding library. Describe bindings the way
you write a C++ class; the metadata comes from the real C++ type structure at
compile time, the same bindings generate a TypeScript definition file (`.d.ts`),
and the runtime is built so that JS can never reach freed C++ memory through it.

```cpp
// Your ordinary C++ class, unchanged
class Player : public Entity {
public:
    Player(std::string name, int level);
    void attack(Entity& target);
    int mHp = 100;
};

// A binding is a specialization, written like the class itself
// (third-party types work too)
template<>
struct Alka::Bind<Player> : Alka::Class<Player> {          // name reflected -> "Player"
    using Extends = Entity;                                 // a real C++ base (static_assert'd)
    static constexpr auto doc = "A controllable player.";
    static constexpr auto naming = Alka::memberPrefixes;    // mHp -> "hp" (default: as written)
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("level") = 1),
        Alka::method<&Player::attack>(Alka::doc("attack a target")),  // -> "attack"
        Alka::prop<&Player::mHp>(Alka::readonly));                    // -> "hp"
};

Alka::install<GameMod>(context);                 // materialize into any v8::Context
std::string dts = Alka::generateDts<GameMod>();  // and generate the .d.ts
```

```ts
// generated game.d.ts (ES-module style also available)
declare namespace game {
    /** A controllable player. */
    class Player extends Entity {
        constructor(name: string, level?: number);

        /** attack a target */
        attack(target: Entity): void;

        readonly hp: number;
    }
}
```

**Documentation:** [Tutorial](docs/Tutorial.md) · [Cookbook](docs/Cookbook.md) ·
[Reference](docs/Reference.md) · [Performance](docs/Performance.md)

## Features

**Declaring bindings**

- **Two equivalent forms** — non-intrusive `Alka::Bind<T>` specialization (binds
  third-party types) or in-class `alkaMembers` / `AlkaExtends`.
- **Compile-time name reflection** — class / method / property / enum names from
  `__FUNCSIG__` / `__PRETTY_FUNCTION__`, as written unless a binding picks a **naming**: composable steps
  (`stripPrefix<"m", "m_">`, `stripSuffix<"_">`,
  `camelCase`, `pascalCase`, `only<NameKind...>(step)`), exact renames for the
  names no rule fits (`rename<"cxx", "js">`), constexpr functions of your own;
  per binding, inherited by subclasses, or project-wide (`ALKA_NAMING`). Any
  declaration can still be named explicitly. Two members ending up with one JS
  name (or one V8 reserves: `constructor`, a class's `name`, `length`,
  `prototype`) are a compile error.
- **Classes, records, modules** — `Alka::Class<T>` (wrappers with identity and a
  prototype), `Alka::Record<T>` (value types ↔ plain JS objects, `interface` in
  the `.d.ts`), `Alka::Module<"path">` with functions, vars, enums, error
  classes, submodules; `installInto` fills any object (a Node addon's `exports`).
- **Metadata** — `doc` / `arg().doc()` / `deprecated` / `meta<"k">(v)`, queryable
  at runtime (`classInfo<T>()`) and emitted as JSDoc.

**Calls**

- **One call pipeline** for functions, methods, constructors, accessors and native
  callables (`Alka::callable(lambda)` makes one at run time, captures and all):
  overloads (declaration order), `arg("dt") = 0.016` defaults,
  `std::string_view` / `const char*` / `std::span<const T>` parameters, a trailing
  `Alka::Rest<T>` (`...parts: T[]`), and injected parameters (`Alka::CallInfo`,
  `v8::Isolate*`, `ConvertCtx&`) that take no JS argument.
- **Return policies** — `policy::borrow / copy / move / take / internalRef`, plus
  `keepAlive<I>` ("this object keeps argument I alive"; `releaseKept` lets go).
- **Errors** — C++ exceptions become JS errors; register exception types (`Alka::error<"NotFound", NotFound>()` /
  `registerError`) to get real JS Error
  subclasses with hierarchy and extra fields; `std::expected<T, E>` returns `T`
  or throws `E`; a JS exception crossing C++ arrives back as the very same object.
- **Cancellation** — an `Alka::AbortSignal` parameter receives a JS
  `AbortSignal` as a `std::stop_token` (poll it, or attach a `stop_callback`).
- **64-bit integers** — `bigint` by default; `int64As<Int64Mode::Number|Auto>` per
  declaration or `ALKA_INT64_MODE` globally; inputs accept both.
- **V8 Fast API** — eligible callables (numbers, bools, strings) get TurboFan
  fast paths automatically, behaving exactly like the regular path (same
  validation and errors); opt out with `Alka::noFast`.
- **Cached hot keys** — property names and private symbols used on every call (iterator steps, record fields, field
  views, `toJSON`, override dispatch) are
  built once per isolate; a virtual JS does not override runs its C++ base
  without a round trip through JS (`tests/Runtime/BenchHotPaths.cpp` measures).

**JS shape**

- **Prototype chain = real inheritance** (multi-level, non-first-base offsets),
  native `instanceof`, static inheritance, JS `class extends` of native classes.
- **Virtual overrides** — declare a trampoline (`using Trampoline = JsAnimal;`,
  `ALKA_OVERRIDE`, or `overrideOr<R, "jsName">` when the JS name differs) and JS
  subclasses override C++ virtuals for C++ callers too; `super` calls reach the
  C++ base without recursion.
- **Idioms** — `iterable<T>()` (`for…of`, spread), `indexer()` (`obj[i]` through
  `size()` + `operator[]`, or any accessors: `indexer<&Grid::cell, &Grid::set,
  &Grid::count>()`), `namedProperties` (`obj.anyName`), `asFunction`
  (instances you can call), `toStringFromFormat<T>()`, `toJSON<T>()`,
  `equals<T>()`, `disposable` (`dispose()` and `using`), static methods and
  properties (`staticMethod`, `staticProp`), `std::generator<T>` results as JS
  iterators; `@@iterator`-style names bind well-known symbols.
- **Identity** — the same C++ object is the same JS object per context (scoped by
  class hierarchy); polymorphic down-typing (`Entity*` that is a `Player`
  surfaces as `Player`).
- **ES modules** — `registerModules<Mods...>(ctx)` makes bound modules importable (`import { Player } from "game"`),
  matching the ES-module `.d.ts`.

**Memory safety**

- **Ownership is explicit and enforced** — JS-created objects are JS-owned;
  by-value / `unique_ptr` / `shared_ptr` / `take` results are owned; a later
  owning hand-over upgrades an existing borrowed wrapper.
- **Strict borrowing** — a `T*` / `T&` result lends JS an object C++ may
  destroy, so it only compiles when someone vouched for the lifetime: the class
  derives from `Alka::Trackable` (destruction on any thread detaches its
  wrappers), declares a `lifetime` (`Static`, or `Manual` with
  `Alka::invalidate`), or the declaration says `policy::borrow`.
- **No dangling views** — views of parts (`polygon[0]`, a field of class type, an
  accessor's result) are *live paths*: found again from their owner on every
  use, so a container that reallocates, shrinks or clears can never leave them
  pointing at freed memory; they keep their owner alive. `weak_ptr` results
  stop working when C++ lets go; `enable_shared_from_this` objects are shared
  automatically.
- **Pins** — a `shared_ptr<T>` handed to C++ (or `Alka::pin`) keeps the JS wrapper
  alive too, so JS-side state (expandos, overrides) is never silently lost.
- **Safe early release** — `dispose()` (and `using`) frees a JS-owned object at
  once, but only after every native frame still using it has returned.
- **No cross-language leaks** — `Alka::Retained<T>` members hold JS values through
  their owner's wrapper (set as a property, a parameter, or `retained.set(this,
  value)` from C++; across contexts, on the owning wrapper), so `closure →
  wrapper → C++ → closure` cycles are collectable -- also once JS comes to own an
  object it first borrowed; contexts are held weakly. `Alka::GcObject<T>` goes
  further: such objects live on V8's C++ heap (cppgc), traced together with JS,
  so cycles through either side are collected.
- **Safe handles** — `Value` / `Object` / `Function` / `Promise` refuse to run off
  their isolate's thread, release safely from any thread, and report "isolate
  disposed" instead of dangling; `WeakValue` observes without keeping alive.
- **Coroutine bindings reject dangerous parameters** at compile time (references,
  views, raw pointers) and keep their receiver alive until they settle (a
  borrowed receiver is refused); an offloaded step keeps its frame alive until
  the worker returns, even across isolate disposal.
- **Script cannot crash the host** — conversions validate receivers and bounds *after* running JS (getters, proxies),
  untrusted lengths are never trusted for
  allocation, and V8 failures (termination, hostile prototypes) become JS errors
  instead of aborts.
- **Checked, not just designed** — the whole suite runs under ASan + UBSan, TSan
  and the MSVC debug heap (`ALKA_TEST_HEAPCHECK`); `fuzz/AlkaFuzz` generates JS
  programs that construct, share, borrow, dispose and destroy objects across
  the boundary (libFuzzer or standalone).

**Async & threads**

- `Alka::Promise<T>` (settle from C++) and `Settler<T>` (settle from any thread;
  an unsettled one keeps the event loop alive).
- `Alka::Task<T>` coroutines become promises; `co_await Alka::await(jsPromise)`,
  `co_await Alka::offload(fn)` (worker thread, resumes on the isolate thread),
  `co_await Alka::resumeOn(dispatcher)` (back to the isolate thread after a
  non-Alka awaitable resumed the coroutine elsewhere).
- `Alka::ThreadSafeFunction<R(Args...)>` — call a JS function from any thread;
  the calls run in order on the isolate thread.
- `Alka::Dispatcher` — post work to the isolate thread from anywhere, with a
  wakeup hook for the host's event loop; `busy()` for exit-when-idle loops.
- Deferred destruction (`setDestructionMode(Deferred)`), so native destructors may
  run JS; native memory reporting (`Bind<T>::externalSize`).

**Conversions** (`Alka::Converter<T>`, specializable): numbers, bool, strings (UTF-8, `u16string`, `wstring`,
`filesystem::path`), `chrono` durations (milliseconds) and `system_clock` time points (`Date`), `vector` / `array` /
`deque` / `list` / `span`, maps (object or `Map`), sets, `optional`, `tuple` /
`pair` / `variant` (`monostate`, `nullptr_t`), typed arrays (copied,
`std::span<T>` in place, or kept zero-copy as `Alka::TypedView<T>`), bytes ↔
`Uint8Array`, zero-copy `Alka::BufferView` (`SharedBufferView` opts in to
`SharedArrayBuffer` memory), `std::function` ↔ JS functions, bound classes by
value / pointer / smart pointer.

**Hosting**

- **Engine** (`<Alka/Engine/...>`) — `Engine` / `ContextScope` (`eval`,
  `evalModule`, `runModuleFile`, `pump`, `runUntilIdle`) give scripts what they
  expect: `console` (Node-style formatting), timers, `queueMicrotask`,
  `structuredClone`, `performance`, `TextEncoder` / `TextDecoder`,
  `EventTarget`, `AbortController`, `reportError`; ES module files (relative
  imports, JSON, `import()`, `import.meta`); a heap limit and a time limit that
  stop a script without stopping the process; unhandled rejections reported;
  startup snapshots (`Alka::snapshot::*`).
- **Debugging** — a DevTools-protocol `Inspector`; `tools/InspectorBridge`
  (target `Alka::InspectorBridge`) serves it to Chrome DevTools and VS Code over
  WebSocket -- loopback only, Host-checked discovery, a random endpoint id --
  e.g. `DemoHost --inspect-brk script.js` waits for DevTools and stops at the
  first line.
- **Node.js and Electron** (`<Alka/Host/Node.hpp>`) — `Alka::Node::attach(context)`
  runs Alka's posted work on Node's event loop, as Node callbacks (promise jobs
  and `process.nextTick` follow), keeps the process alive while work is in
  flight, and releases everything with the environment, worker threads
  included. Electron addons build with Electron's own toolchain and libc++
  (`ci/electron-addon.sh`, [Cookbook](docs/Cookbook.md#an-electron-addon)).

## Platforms

Tested locally on:

|                         | Compilers                                         | V8                                                                          |
|-------------------------|---------------------------------------------------|-----------------------------------------------------------------------------|
| Windows                 | MSVC 19.5x (Debug, Release), clang-cl 23 (+ ASan) | 13.0 (NuGet)                                                                |
| Linux                   | GCC 15, clang 21 (+ ASan/UBSan, TSan)             | 12.4 (Ubuntu's libnode), 13.6 (Debian's libnode, C++-heap classes included) |
| Node.js addons, Linux   | GCC 15, clang 21 (+ ASan/UBSan)                   | 12.4, 13.6, 14.6 (Node 22, 24, 26)                                          |
| Node.js addons, Windows | MSVC                                              | 12.4, 13.6 (Node 22, 24)                                                    |
| Electron addon, Linux   | Chromium's clang 24, Electron's libc++            | 15.6 (Electron 45: pointer compression, sandbox)                            |
| compile only            | GCC 15, clang 21                                  | 14.6 headers; 15.6 headers (Electron's ABI)                                 |

`.github/workflows/ci.yml` runs the same suites on GitHub's runners, plus an
experimental macOS job (Node addons with Apple clang) that has never been run
outside CI.

## Layout

| Header folder   | Contents                                                                                                   |
|-----------------|------------------------------------------------------------------------------------------------------------|
| `Alka/Alka.hpp` | umbrella (everything but `Engine/` and `Host/`)                                                            |
| `Alka/Core/`    | V8-agnostic foundations: errors, name reflection, traits                                                   |
| `Alka/Binding/` | the declaration DSL: `Bind`, members, specs, policies, idioms, `Overridable`, `Gc`                         |
| `Alka/Convert/` | `Converter<T>` and the built-in conversions, `Record`, buffers, typed arrays, `chrono`                     |
| `Alka/Js/`      | C++ handles to JS: `Value`, `Function`, `Promise`, `Task`, `Retained`, `ThreadSafeFunction`, `AbortSignal` |
| `Alka/Runtime/` | materialization: `install`, modules, lifetime, `Dispatcher`, errors, snapshots, `Watchdog`                 |
| `Alka/Ts/`      | `.d.ts` generation                                                                                         |
| `Alka/Engine/`  | optional self-hosting layer (needs libplatform)                                                            |
| `Alka/Host/`    | running inside an existing host: Node.js / Electron                                                        |
| `Alka/Detail/`  | internals                                                                                                  |

`tools/` holds companions outside the library (`InspectorBridge`, golden-file
updates); `examples/DemoHost` is a runnable host (script, ES module, REPL,
`.d.ts` generation, `--inspect-brk` debugging); `benchmarks/AlkaBench` measures
Alka against hand-written V8 bindings; `fuzz/AlkaFuzz` fuzzes the boundary;
`tests/Node` runs Alka inside real Node releases.

## Build (bring your own V8)

Alka never downloads or builds V8; the host supplies it through one CMake target
named by `ALKA_V8_TARGET`, carrying the V8 headers, the matching ABI macros, and
the V8/Node import libs on its INTERFACE. Alka links it PUBLIC, so all three
propagate to Alka and to every consumer.

```cmake
# from source
set(ALKA_V8_TARGET YourV8::Target)   # e.g. QQNT::QQNT
set(ALKA_BUILD_ENGINE OFF)              # drive the host's existing isolate
FetchContent_MakeAvailable(Alka)        # or add_subdirectory(Alka)
target_link_libraries(yourTarget PRIVATE Alka::Alka)

# or installed: a source package, compiled in your project against your V8
set(ALKA_V8_TARGET YourV8::Target)
set(ALKA_WITH_ENGINE ON)                # optional: Engine / ContextScope
find_package(Alka 0.2 CONFIG REQUIRED)
target_link_libraries(yourTarget PRIVATE Alka::Alka)
```

- The package installs without V8: `cmake -DALKA_PACKAGE_ONLY=ON` then
  `cmake --install`; or `vcpkg install alka --overlay-ports=<Alka>/ports`.
- `ci/` has ready-made V8 targets: the `v8-v143-x64` NuGet package on Windows (`NugetV8.cmake`) and Ubuntu's
  `libnode-dev` (`LibnodeV8.cmake`), plus
  `fetch-v8-headers.sh` for the upstream headers a Node release leaves out (`v8-fast-api-calls.h`, …).
- `tests/Node/CMakeLists.txt` builds Alka into a Node addon against a Node
  release's headers.

Requires C++23 (`std::expected`, `std::format`, coroutines): MSVC ≥ 14.5x,
clang ≥ 20 or GCC ≥ 15.

**ABI macros** — every TU that includes `<v8.h>` must see the exact macro set (pointer compression / sandbox /
`V8_ENABLE_CHECKS` / …) of the V8 it links
against, or object layouts diverge and V8 aborts at startup. Those macros ride
`Alka::Alka` PUBLIC-ly, so never include `<v8.h>` without going through it.

Compile-time knobs (`Config.hpp`): `ALKA_INT64_MODE`, `ALKA_FAST_API`,
`ALKA_STRICT_BORROW`, `ALKA_NAMING` / `ALKA_NAMING_HEADER`,
`ALKA_ENUM_RANGE_MIN/MAX`, `ALKA_CONTEXT_EMBEDDER_SLOT` /
`ALKA_CONTEXT_EMBEDDER_PROBES`, `ALKA_HAS_CPPGC`, `ALKA_CPPHEAP_TAG`,
`ALKA_EXTERNAL_TAG`. Set them on the `Alka` target (`PUBLIC`), so every
translation unit sees the same values.

Tests (`ALKA_BUILD_TESTS`) build `AlkaTests` (GoogleTest). In MSVC Debug builds
`ALKA_TEST_HEAPCHECK=1` validates the whole debug heap on every allocation (`=N`: every N-th) — the memory-safety run
for setups where AddressSanitizer
cannot be used, such as Debug-only V8 packages. `ALKA_BUILD_BENCHMARKS` builds
`AlkaBench`; `ALKA_BUILD_FUZZERS` builds `AlkaFuzz` (a libFuzzer target with
`ALKA_FUZZ_LIBFUZZER`, otherwise a standalone random / replay driver).

## Embedding

The core is host-agnostic — hand it any `v8::Local<v8::Context>`:

```cpp
Alka::install<GameMod, CoreMod>(context);   // materialize (each context gets the modules you name)
Alka::Dispatcher dispatcher(isolate);       // cross-thread work: wire setWakeup() to your loop,
dispatcher.drain();                         //   drain() it on the isolate thread
Alka::disposeIsolate(isolate);              // before Isolate::Dispose -- V8 runs no teardown weak callbacks
```

Inside Node.js or Electron, the addon's entry point is all there is:

```cpp
NODE_MODULE_INIT(/* exports, module, context */) {
    Alka::Node::attach(context);                     // Node's event loop runs Alka's work
    Alka::installInto<GameMod>(context, exports);    // module.exports = { Player, ... }
}
```

## Performance

Per operation, next to hand-written V8 bindings doing the same work with the
type checks a careful binding makes (MSVC Release, V8 13.0, `AlkaBench`):

| Operation                              | Alka   | hand-written |
|----------------------------------------|--------|--------------|
| `add(int, int)` (Alka on the Fast API) | 14 ns  | 12 ns        |
| the same, both on regular callbacks    | 37 ns  | 13 ns        |
| property get                           | 35 ns  | 14 ns        |
| `string_view` in (16 chars)            | 57 ns  | 57 ns        |
| string in + out                        | 72 ns  | 96 ns        |
| array of 1000 numbers in               | 6.9 µs | 27 µs        |
| new object                             | 466 ns | 172 ns       |
| C++ calls a JS function                | 77 ns  | 42 ns        |

The difference buys identity (one JS object per C++ object), ownership records
and full validation of arguments and receivers. Where the time goes and how to
cut it: [Performance](docs/Performance.md).

## Customization points

| Point                             | Purpose                                                   |
|-----------------------------------|-----------------------------------------------------------|
| `Alka::Converter<T>`              | JS ↔ C++ conversion for any type (one direction is fine)  |
| `Alka::TsTypeName<T>`             | that type's `.d.ts` mapping                               |
| `Alka::tsType<"X">`               | per-declaration TS type override                          |
| `Bind<T>::naming` / `ALKA_NAMING` | how reflected names become JS names (default: as written) |
| `Alka::EnumRange<E>`              | enum reflection scan range (default −128..128)            |
| `Bind<T>::externalSize`           | native memory a JS-owned `T` accounts for                 |
| `TsGenOptions::mMetaRenderers`    | custom meta → JSDoc tag rendering                         |

## Limitations

- Bound callables must be usable as NTTPs: member / function pointers or
  captureless lambdas (`Alka::callable` wraps anything at run time). MSVC
  cannot derive names of *virtual* member functions — give them explicit names (`method<"speak", &Animal::speak>()`).
- Cycles that run through C++ ownership (C++ holds a JS value that references the
  object owning that C++ state) are not collected; `Retained<T>` avoids the common
  case where the owner is JS-owned, and `GcObject<T>` classes avoid it entirely.
- Borrowed objects destroyed while a JS call into them is running, and C++ taking
  ownership of a JS-created object behind Alka's back (`shared_from_this` instead
  of `Alka::pin`), remain the host's responsibility.
- C++ cannot take *sole* ownership of a JS-created object: `std::unique_ptr<T>`
  parameters are refused at compile time (take `std::shared_ptr<T>`).
- Startup snapshots do not carry native objects, functions made at run time from
  C++ callables (`std::function` results, `Alka::callable`), iterator objects or
  synthetic modules.
- `GcObject<T>` needs V8 13+ and, on Windows, a V8 DLL used with
  `USING_V8_SHARED`; the NuGet V8 package cannot be used that way.
- The Fast API needs a V8 whose fast calls can throw (about 12.6 on); older ones
  take the regular path.
- V8 14 and 15 are verified inside Node 26 and Electron 45 (and compiled
  against 14.6 / 15.6 headers), not as standalone embedders.
- Electron addons cannot use `GcObject<T>` (Electron exports no cppgc
  allocation internals) and need Electron's toolchain; only Linux x64 has
  been tried.
- Toolchains: clang 22 miscompiles `std::format` with a literal format string *inside a bound lambda* (bind a function
  or use `std::vformat`); clang-cl's
  ASan crashes rethrowing exceptions out of `std::generator`; clang ≥ 17
  against a V8 built by GCC (Linux Node releases and their addons,
  distributions' libnode) needs `COMPONENT_BUILD` for `GcObject` classes (see
  `<Alka/Binding/Gc.hpp>`; `ci/LibnodeV8.cmake` sets it).

## Migrating from 0.1

- Headers moved into folders (see Layout); `Function.hpp` is now `Js/Function.hpp`
    + `Js/Value.hpp`, `Fwd.hpp` folded into `Config.hpp` / `Binding/Bind.hpp`.
      `<Alka/Alka.hpp>` no longer includes the Engine layer.
- `Alka::internalRef` → `Alka::policy::internalRef`.
- `ConvertCtx` has a constructor (`{isolate, context}` still works); converters
  take `ConvertCtx&`.
- `Object::set` and `Promise::resolve/reject` return `Result<void>`.
- Strict borrowing is on (`ALKA_STRICT_BORROW=1`): `T*` / `T&` results of classes
  that are not `Trackable`, declare no `lifetime` and are not marked
  `policy::borrow` no longer compile.
- A `shared_ptr<T>` taken from a JS-created object now pins its wrapper; the
  object is released by the GC after C++ lets go.
- A JS exception rethrown through C++ keeps its original object (no `Error:`
  message prefix) -- also one thrown while an argument converts (a getter, a
  proxy trap), which used to become a `TypeError`.
- `install<Mods...>(context)` takes no options (`InstallOptions` is gone).
- Reflected names are used as written: `mHp` stays `mHp`. For 0.1's `mHp → hp`,
  give the binding `static constexpr auto naming = Alka::memberPrefixes;` (or the
  whole build `-DALKA_NAMING=Alka::memberPrefixes`). `NamePolicy<>` is gone; a
  naming covers it, per binding.
- `ALKA_OVERRIDE(Ret, Base, name)` takes the C++ method name and looks the
  override up under the name the class's naming gives it.
- Two members with one JS name, and V8's own names (`constructor`; `name`,
  `length`, `prototype` on a class), no longer compile -- in a class they used to
  abort at install time, in a module the first won. Installing a name another
  module defined differently throws, and so does registering two modules under
  one path (both used to keep the first silently).
- In the `.d.ts`, `const char*` parameters are `string | null`.
- A non-public in-class binding name (`alkaMembers`, `AlkaExtends`, ...) is a
  compile error instead of being ignored.
- Engine: `createIsolate(IsolateOptions)`, `ContextScope(isolate, ContextOptions)`,
  `eval(source, name, EvalOptions)`; `runUntilIdle` returns whether no error went
  unhandled. Contexts get `console`, timers and the web globals by default (`ContextOptions` turns them off).
- `ALKA_CONTEXT_EMBEDDER_SLOT` moved to 64 and is probed: a host that overrode it
  to dodge a collision can usually drop the override.

## License

MIT
