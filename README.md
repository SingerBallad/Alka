# Alka

A declarative C++23 ↔ V8 binding library. Describe bindings the way you write a
C++ class; the metadata comes from the real C++ type structure at compile time,
and the same bindings generate a TypeScript definition file (`.d.ts`)
automatically.

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
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("level") = 1),
        Alka::method<&Player::attack>(Alka::doc("attack a target")),  // -> "attack"
        Alka::prop<&Player::mHp>(Alka::readonly));                    // mHp -> "hp"
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

## Features

- **Two equivalent forms** — non-intrusive `Alka::Bind<T>` specialization (binds
  third-party types) or in-class `alkaMembers` / `AlkaExtends`.
- **Compile-time name reflection** — class / method / property / enum names
  extracted from `__FUNCSIG__` / `__PRETTY_FUNCTION__`; the `mHp → hp` policy is
  replaceable (`NamePolicy<>`) and overridable per declaration.
- **JS shape mirrors C++** — prototype chain = real inheritance (multi-level,
  non-first-base offsets), native `instanceof`, static inheritance, and JS
  `class extends` of native classes (`super()`, overrides, passing back into C++).
- **Polymorphic down-typing** — a returned `Entity*` that is really a `Player`
  surfaces as `Player`.
- **Object identity cache** — the same C++ object is the same JS object within a
  context (`===` holds; expando properties survive round-trips).
- **Lifetime** — JS-constructed objects are JS-owned (weak GC release); `T*`/`T&`
  returns borrow; by-value / `unique_ptr` transfer ownership; `shared_ptr` shares
  both ways.
- **Converters** — numbers (int64 ↔ BigInt), strings, `vector`/`array` ↔ Array,
  maps ↔ object/Map, set ↔ Set, `optional` ↔ `T | undefined`, `tuple`/`variant`,
  byte spans ↔ Uint8Array, `std::function` ↔ JS function — all through a
  specializable `Alka::Converter<T>`.
- **Overloads & defaults** — declaration-order dispatch; `arg("dt") = 0.016`
  fills missing / `undefined` arguments and emits `dt?: number` + `@default`.
- **Metadata** — `doc` / `arg().doc()` / `deprecated` / custom `meta<"k">(v)`,
  queryable at runtime (`classInfo<T>()`) and emitted as JSDoc.
- **Calling back into JS** — `Alka::Function<R(Args...)>` (`tryCall` →
  `std::expected`, `call` → throws `JsException`), `Alka::Value` / `Object` handles.

## Build (bring your own V8)

Alka never downloads or builds V8; the host supplies it through one CMake target
named by `ALKA_V8_TARGET`, carrying the V8 headers, the matching ABI macros, and
the V8/Node import libs on its INTERFACE. Alka links it PUBLIC, so all three
propagate to Alka and to every consumer.

```cmake
set(ALKA_V8_TARGET    YourV8::Target)   # e.g. QQNT::QQNT
set(ALKA_BUILD_ENGINE OFF)              # drive the host's existing isolate
FetchContent_MakeAvailable(Alka)        # or add_subdirectory(Alka)
target_link_libraries(yourTarget PRIVATE Alka::Alka)
```

Requires C++23 (`std::expected`, `std::format`): MSVC ≥ 14.5x or clang ≥ 20.

**ABI macros** — every TU that includes `<v8.h>` must see the exact macro set
(pointer compression / sandbox / …) of the V8 it links against, or object layouts
diverge and `V8::Initialize()` aborts at startup. Those macros ride `Alka::Alka`
PUBLIC-ly, so never include `<v8.h>` without going through it.

## Embedding

The core is host-agnostic — hand it any `v8::Local<v8::Context>`:

```cpp
Alka::install<GameMod, CoreMod>(context);   // materialize (a context subset is selectable)
Alka::disposeIsolate(isolate);              // before Isolate::Dispose -- V8 runs no teardown weak callbacks
```

With `ALKA_BUILD_ENGINE=ON` (default) and a libplatform-carrying V8 target,
`Alka::Engine` / `Alka::ContextScope` self-host V8 (ICU → platform → Initialize)
for standalone use — this is what Alka's own tests and `DemoHost` use.

## Customization points

| Point | Purpose |
|---|---|
| `Alka::Converter<T>` | JS ↔ C++ conversion for any type (one direction is fine) |
| `Alka::TsTypeName<T>` | that type's `.d.ts` mapping |
| `Alka::tsType<"X">` | per-declaration TS type override |
| `Alka::NamePolicy<>` | global naming policy (default strips the `m` prefix) |
| `Alka::EnumRange<E>` | enum reflection scan range (default −128..128) |
| `TsGenOptions::mMetaRenderers` | custom meta → JSDoc tag rendering |
| `ALKA_CONTEXT_EMBEDDER_SLOT` | context embedder slot, if it clashes with the host |

## Limitations

- Bound callables must be usable as NTTPs: member / function pointers or
  captureless lambdas.
- C++ virtual functions are not dispatched to JS overrides (a JS override affects
  only JS-side calls).
- `Alka::Value` / `Function` handles are single-isolate and must be released
  before the isolate.
- A bound type's destructor must not call back into JS (the GC second pass is not
  a script-safe point).
- **clang 22 note:** `std::format` / `std::print` with a literal format string
  *inside a bound lambda* miscompiles under standalone LLVM clang 22 (an
  undefined `_Compile_time_parse_format_specs`); bind a free / member function or
  use `std::vformat` instead. clang ≤ 20 and MSVC are unaffected.

## License

MIT
