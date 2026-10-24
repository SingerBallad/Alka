# Alka API reference

Alka 0.2.0. Covers the public API under `include/Alka` (namespace `Alka` unless stated); internals in `Alka/Detail/` appear only where users meet them. "Isolate thread" is defined under [Thread rules](#thread-rules). Task-oriented material: [Tutorial.md](Tutorial.md), [Cookbook.md](Cookbook.md).

## 1. Overview

- **Declare**: describe a C++ type with a `Bind<T>` specialization deriving from `Class<T>`, `Record<T>` or `Module<"path">` (or in-class `alkaMembers`). `Alka::members(...)` is a `constexpr` tuple of descriptors; callables are template arguments and names are reflected at compile time.
- **Materialize**: `install<Mods...>(context)` (global namespaces), `installInto<Mod>(context, obj)`, `registerModules<Mods...>(context)` (ES modules) or `registerClass<Ts...>(isolate)`. Class templates are created once per isolate, constructors once per context.
- **Convert**: every value crossing the boundary goes through `Converter<T>` (`fromJs` / `toJs`, either may be absent); `TsTypeName<T>` names it in the `.d.ts` that `generateDts<Mods...>()` emits.
- **Hold JS**: `Value`, `Object`, `Array`, `Function<Sig>`, `Promise<T>`, `Task<T>`, `Retained<T>` and `ThreadSafeFunction<Sig>` are thread-checked handles that may be dropped on any thread.
- **Host**: drive your own isolate (drain a `Dispatcher`, call `disposeIsolate`), use the optional `Engine` layer, or run inside Node.js / Electron (`Node::attach`).
- `<Alka/Alka.hpp>` includes everything except `Engine/*`, `Host/Node.hpp` and `Runtime/Snapshot.hpp`. C++23; V8 API differences from 12.4 to 15 are absorbed in `Detail/Compat.hpp`.

## 2. Declaring bindings

### Class bindings: `Bind<T>`, `Class<T>`

The non-intrusive customization point (works for third-party types) or the equivalent in-class form; JS gets wrapper objects with identity, a prototype and `instanceof`. Header: `<Alka/Binding/Bind.hpp>`

```cpp
template<> struct Alka::Bind<Player> : Alka::Class<Player> {   // or Class<Player, "Hero">
    using Extends = Entity;                                     // optional
    static constexpr auto doc = "A controllable player.";       // optional
    static constexpr auto naming = Alka::memberPrefixes;        // optional (Naming below)
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("level") = 1),
        Alka::method<&Player::attack>(),                        // "attack"
        Alka::prop<&Player::mHp>(Alka::readonly));              // "hp" (without the naming: "mHp")
};
class Player : public Entity {                                  // in-class form (classes only)
public:                                                         // the alka* members must be public
    using AlkaExtends = Entity;                                 // all optional except alkaMembers
    static constexpr auto alkaClassName = "Hero";  static constexpr auto alkaDoc = "...";
    static constexpr auto alkaLifetime = Alka::Lifetime::Manual;  std::size_t alkaExternalSize() const;
    static constexpr auto alkaNaming = Alka::memberPrefixes;
    static constexpr auto alkaMembers = Alka::members(/* ... */);
};
```

- The JS name defaults to the unqualified C++ name through the class's naming (`NameKind::Type`); it must be an identifier, so template instantiations need `Class<T, "Name">`.
- `members` is required; optional: `Extends`, `Trampoline`, `lifetime`, `doc`, `naming`, `externalSize`. Records, modules and trampolines need `Bind<T>`; when both forms exist, `Bind<T>` wins.
- An in-class name that is not public is a compile error once the class is installed, exported with `type<T>` or put into a `.d.ts` (instead of being ignored); so is `type<T>` of a type without any binding.
- `doc` is the only class- or module-level metadata; members take `doc`, `deprecated` and `meta` ([Specs](#specs)).
- Concepts: `Bound<T>`, `BoundClass<T>` (bound, neither module nor record), `BoundRecord<T>`, `BoundModule<T>`.

### Value types: `Record<T>`

Converts to and from a plain JS object field by field; no wrapper, no identity. Header: `<Alka/Binding/Bind.hpp>`, `<Alka/Convert/Records.hpp>`

```cpp
template<> struct Alka::Bind<Vec2> : Alka::Record<Vec2> {      // Record<Vec2, "Vector"> renames
    static constexpr auto members = Alka::members(Alka::prop<&Vec2::x>(), Alka::prop<&Vec2::y>());
};
```

- Members must all be field props; `T` must be default-constructible (static_asserts).
- From JS: an object (not an array), fields read by name. A missing or mistyped field is an error naming the key, except `std::optional` fields that are `undefined` (they keep their default). To JS: a new plain object; empty `std::optional` fields are omitted (absent, not `undefined`).
- Exported with `Alka::type<T>`: nothing is installed at run time; the `.d.ts` gets an `interface` (`std::optional` fields as `name?:`). `readonly` only affects the `.d.ts`.

### Modules: `Module<"path">`

A namespace of functions, classes, records, enums, error classes, constants and submodules, bound on a tag type. Header: `<Alka/Binding/Bind.hpp>`

```cpp
struct GameMod;
template<> struct Alka::Bind<GameMod> : Alka::Module<"game"> {     // dotted paths: "game.ai"
    static constexpr auto doc = "Game API";
    static constexpr auto members = Alka::members(
        Alka::type<Player>, Alka::enumType<Color>(), Alka::fn<&spawn>(Alka::arg("name")),
        Alka::error<"GameError", GameError>(), Alka::var<"VERSION">(3), Alka::submodule<GameAiMod>);
};
```

- `install` creates the dotted path on the global object (each level read-only, non-deletable) and defines the members as read-only, non-deletable properties. Installing a module again into the same context (or `installInto` the same object) changes nothing.
- A submodule installs under its own path, not relative to its parent; `installInto` nests submodules by the last path segment; as ES modules they are separate modules.
- Modules with one path share its namespace. A name two of them define (differently) there is a compile error within one `install<...>` / `generateDts<...>`, and a `JsException` when installed by separate calls; `registerModules` refuses two modules with one path the same ways.

### Inheritance: `Extends`

Mirrors one real C++ base class in JS. Header: `<Alka/Binding/Bind.hpp>`

```cpp
using Extends = Entity;               // in Bind<Player>; in-class: using AlkaExtends = Entity;
using B = Alka::ExtendsOf<Player>;    // Entity (void when none)
```

- static_asserts: a real C++ base of `T`, not `T` itself, and itself bound.
- JS: prototype chain, static inheritance (statics reachable through the subclass), native `instanceof`; `class X extends game.Player` works in JS (its constructor must call `super()`).
- Non-first bases are fine (pointers are adjusted). A pointer to a polymorphic object surfaces as its most-derived bound class installed in the isolate. `lifetime` and the indexer / named-property / call handlers are inherited.

### Virtual overrides: `Trampoline`

Lets JS subclasses override C++ virtual functions, for C++ callers too. Header: `<Alka/Binding/Overridable.hpp>`

```cpp
struct JsAnimal : Animal, Alka::Overridable<Animal> {
    using Animal::Animal;
    std::string speak() override { return ALKA_OVERRIDE(std::string, Animal, speak); }
    int legs() override { return ALKA_OVERRIDE_PURE(int, Animal, legs); }              // pure virtual
    std::string label() override {                                                      // JS name differs
        return Alka::overrideOr<std::string, "displayName">(this, [&] { return Animal::label(); });
    }
};
template<> struct Alka::Bind<Animal> : Alka::Class<Animal> {
    using Trampoline = JsAnimal;
    static constexpr auto members = Alka::members(Alka::ctor<>(), Alka::method<"speak", &Animal::speak>());
};
```

- JS-constructed instances are trampolines: `ctor<Args...>` must match a trampoline constructor (inherit them with `using T::T;`); `T` may be abstract but needs a virtual destructor; the trampoline derives from `T`.
- A C++ virtual call runs the JS override when one exists, else the C++ base directly (no trip through JS); `super.speak()` in JS reaches the C++ base without recursing. Overrides run only on the isolate thread (elsewhere the C++ base runs).
- A JS exception thrown by an override, or a return value that does not convert, surfaces as `JsException`. Bound objects passed by reference or pointer are lent to the override (no copies); other arguments use `toJs`.
- Keep the object alive from C++ through a `shared_ptr` obtained from Alka (a `shared_ptr<T>` parameter or `pin`): that also keeps its JS side, the overrides, alive.
- `ALKA_OVERRIDE(Ret, Base, name, args...)` takes the C++ method name; the JS override is looked up under what `Base`'s naming makes of it (a method bound under another name needs `overrideOr`). `ALKA_OVERRIDE_PURE` throws `JsException("Animal.legs is not implemented")` without a JS override. Macro-free, with the JS name as written: `overrideOr<R, "jsName">(this, base, args...)`, `overrideOr<R>(this, jsName, base, args...)`.
- MSVC cannot reflect names of virtual member functions: name them. Not allowed on `GcObject` classes.

### `Lifetime` and native memory

How `T*` / `T&` objects lent to JS stay valid ([Strict borrowing](#ownership-and-strict-borrowing)), and how much native memory a JS-owned `T` accounts for. Header: `<Alka/Binding/Bind.hpp>`, `<Alka/Detail/Invoke.hpp>`

```cpp
enum class Lifetime : std::uint8_t { Manual, Static };
static constexpr auto lifetime = Alka::Lifetime::Manual;                 // in Bind<T>; in-class: alkaLifetime
static std::size_t externalSize(const Mesh& m) { return m.bytes(); }    // in Bind<Mesh>; in-class: alkaExternalSize()
```

- `Manual`: C++ calls `Alka::invalidate(isolate, ptr)` before destroying an object JS may hold. `Static`: lent objects outlive the isolate (singletons, globals, arenas). Inherited by bound subclasses; not needed for `Trackable` types.
- `externalSize` is reported to V8 (GC pressure) while JS owns the object; default none. See `updateExternalSize` and `reportedExternalMemory` in [Lifetime tools](#lifetime-tools).

### Naming and reflection

Reflected names -- class, enum, enumerator and member names taken from the declarations -- go through the binding's naming. Header: `<Alka/Binding/Naming.hpp>`, `<Alka/Core/NameOf.hpp>`

```cpp
enum class NameKind : std::uint8_t { Type, Method, StaticMethod, Property, StaticProperty, Function, EnumValue };
static constexpr auto naming = Alka::naming(steps...);    // in Bind<T> (class, record, module); in-class: alkaNaming
Alka::stripPrefix<"m", "m_">     Alka::stripSuffix<"_">     Alka::camelCase     Alka::pascalCase
Alka::only<Alka::NameKind::Property, ...>(step)            Alka::rename<"cxxName", "jsName">
Alka::naming([](Alka::NameKind kind, std::string_view name) -> Alka::NameText { ... })   // or (std::string_view)
Alka::verbatim  /* naming(): the default */    Alka::memberPrefixes  /* m / m_ props, s / s_ statics */
naming.apply(kind, "cxxName")  /* the JS name, an Alka::NameText (256 chars; constexpr, also at run time) */
Alka::typeNameOf<Game::Player>  /* "Player" */      Alka::qualifiedTypeNameOf<Game::Player>  /* "Game::Player" */
Alka::valueNameOf<&Player::attack>  /* "attack" */  Alka::valueNameOf<Color::Red>  /* "Red" */
Alka::enumEntriesOf<Color>  /* EnumEntry{mValue, mName}..., ascending */   Alka::enumNameOf(Color::Red)  /* or "" */
template<> struct Alka::EnumRange<Flags> { static constexpr auto min = 0; static constexpr auto max = 1024; };
```

- Whose naming: a class's names the class and its members; a module's its functions, enum types and enumerators (exported classes keep their own); a record's its fields. A class without one takes its bound base's (`Extends`), and every binding without one the project's: `ALKA_NAMING`, an expression naming a naming (`-DALKA_NAMING=Alka::memberPrefixes`; one of your own goes in a header named by `ALKA_NAMING_HEADER`), `Alka::verbatim` unless set. Set it on the `Alka` target `PUBLIC`ly: translation units disagreeing would name things differently (MSVC's linker refuses the mix).
- Steps run in order on the current name. `stripPrefix<P...>`: the first prefix that fits -- one ending in a letter or digit only before a capital (`mHp`, not `make`) -- goes, and a name that started lowercase starts lowercase again (`mURLPath` -> `urlPath`; `FVector` -> `Vector`). `stripSuffix<S...>`: the first suffix that fits. `camelCase` / `pascalCase` keep leading `_` / `$` and trailing `_`, split words at `_` (a SCREAMING word lowercases first) and lowercase or capitalize the first word, a leading acronym as a whole (`HTTPServer` -> `httpServer`, `IDs` -> `ids`).
- Kinds: `camelCase` applies to methods, properties and functions, `pascalCase` to types, every other step to all kinds; `only<Kinds...>(step)` applies `step` to exactly those (`only<NameKind::EnumValue>(camelCase)`).
- `rename<"cxx", "js">` matches the C++ name itself, wherever it stands, and its result is final; renames are tried (in order, nested ones too) before any other step.
- A callable step must be usable in constant expressions and return something a `std::string_view` can be made from: `std::string_view` (a part of its argument too), a literal, an `Alka::NameText` -- or `std::string` where the standard library allows it at compile time (not libstdc++ under clang).
- The result must be an identifier (members: or an `@@symbol` name), else a compile error names the C++ name. Explicit names (`method<"name", ...>`, `Class<T, "Name">`, `enumType<"Name", E>`, `Module<"path">`) never pass through a naming.
- An explicit name is required (static_assert) for lambdas and MSVC virtual member functions.
- Two members of a binding with one JS name -- prototype names (methods, props, `dispose` and `[Symbol.dispose]`), class names (statics, class vars) and module names (functions, classes, records, enums, errors, submodule segments) each count separately, as do an enum's enumerators -- are a compile error naming the binding and the name, as are names V8 keeps: `constructor` on the prototype; `prototype`, `name`, `length`, `arguments`, `caller` on the class.
- Enum reflection scans `[EnumRange<E>::min, max]` (default `ALKA_ENUM_RANGE_MIN/MAX`, -128..128), clamped to the underlying type.
- Names starting with `@@` bind symbols on `method`, `staticMethod`, `prop` and `staticProp` (`[Symbol.x]` in the `.d.ts`): well-known ones (`@@iterator`, `@@asyncIterator`, `@@dispose`, `@@asyncDispose`, `@@toPrimitive`, `@@toStringTag`, ...; skipped when the V8 lacks the symbol) or registry symbols (`@@nodejs.util.inspect.custom` is `Symbol.for("nodejs.util.inspect.custom")`).
- `FixedString<N>` is the compile-time string used for names (`view()`, `size()`, `c_str()`, `operator+`).

## 3. Members

Header: `<Alka/Binding/Members.hpp>` (idioms: `<Alka/Binding/Idioms.hpp>`; `error`: `<Alka/Runtime/Errors.hpp>`). A member in the wrong kind of binding is a compile error.

| Factory | Class | Module | Record |
|---|---|---|---|
| `ctor`, `method`, `staticMethod`, `staticProp`, idioms | yes | | |
| `prop` | yes | | field props only |
| `var` | yes (static constant) | yes | |
| `fn`, `type`, `enumType`, `error`, `submodule` | | yes | |

Bound callables are template arguments: function pointers, member function pointers or captureless lambdas (`BindableCallableValue`); generic lambdas, ref-qualified member functions and overloaded call operators are refused. Where a member is expected, a non-member callable takes the object first (`T&`, `const T&`, `T*`, or a base of `T`).

### `ctor`

```cpp
Alka::ctor<Args...>(specs...)       // ctor<std::string, int>(Alka::arg("name"), Alka::arg("level") = 1)
```

- `Args` are the constructor's parameter types and drive conversion; static_assert when `T` (or its trampoline) has no matching constructor. Several `ctor`s form an overload set (declaration order, no `arg()` defaults).
- `new` is required; without any `ctor` the class cannot be constructed from JS (`TypeError`). JS-constructed objects are owned by their wrapper (held in a `shared_ptr`).

### `method`, `staticMethod`, `fn`

```cpp
Alka::method<&T::f>(specs...)                            // reflected name "f"
Alka::method<"name", &T::f>(specs...)                    // explicit name
Alka::method<"area", &T::area, &T::areaScaled>()         // overload set
Alka::method<"twice", [](const T& self, int n) { return self.v * n; }>()
Alka::staticMethod<&T::create>()     Alka::staticMethod<"of", &makeT>()         // on the constructor
Alka::fn<&spawn>()      Alka::fn<"add", [](int a, int b) { return a + b; }>()    // module functions
```

- Methods live on the prototype (non-enumerable); `this` must be a live wrapper of `T` or a subclass and is checked again after argument conversion (which may run JS).
- `staticMethod` and `fn` take free / static functions or captureless lambdas, no member function pointers. Static methods are inherited by bound subclasses.
- Overload sets: candidates whose JS arity range fits are tried in declaration order; the first whose arguments all convert runs; if none does: `TypeError: no overload of T.name matches the given arguments (number, string)`. `arg()` defaults are not allowed on overload sets.
- A function's `length` is its number of required JS parameters.

### `prop`, `staticProp`

```cpp
Alka::prop<&T::mHp>(specs...)                         // field: read / write, named "mHp" (naming)
Alka::prop<"hp", &T::hp>(specs...)                    // getter only: read-only
Alka::prop<"hp", &T::hp, &T::setHp>(specs...)         // getter + setter
Alka::prop<"size", [](const T& t) { return t.items.size(); }>()
Alka::staticProp<&Config::sDebug>()                   // static variable, live ("sDebug"); const: read-only
Alka::staticProp<"version", &versionOf>()             // getter (free function or lambda)
Alka::staticProp<"level", &getLevel, &setLevel>()     // getter + setter
```

- `prop` is an accessor on the prototype, non-enumerable (`Object.keys` does not list it; see `toJSON`). Writable when a setter exists, `readonly` is not given and (fields) the field is not `const`.
- A field of bound-class type is returned as a view that keeps its owner alive and dies with it (default `policy::internalRef`). `bool`, floating-point and up-to-32-bit integer fields are read directly.
- `staticProp` lives on the constructor (reachable through bound subclasses). A static variable of bound-class type is lent as is (`policy::borrow`).

### `var`, `type`, `submodule`

```cpp
Alka::var<"VERSION">(3)        Alka::var<"NAME">("alka", Alka::doc("product name"))
Alka::type<Player>             Alka::submodule<GameAiMod>         // variable templates: no (), no specs
```

- `var`: a constant converted with `toJs` per context; in a module a read-only property, in a class a read-only, non-deletable property of the constructor (`static readonly` in the `.d.ts`). The value must be usable in a `constexpr` member list; the name must be an identifier.
- `type<T>` exports a bound class (its per-context constructor, named `boundNameOf<T>()`) or a record (`.d.ts` interface only), documented by `Bind<T>::doc`. `submodule<Tag>`: see [Modules](#modules-modulepath).

### `enumType`, `error`

```cpp
Alka::enumType<Color>(specs...)              Alka::enumType<"Colour", Color>(specs...)
Alka::error<"NotFound", NotFound>(specs...)  Alka::error<"Missing", Missing, NotFound>()   // extends NotFound
```

- `enumType` installs a frozen object mapping names to values and values to names (`Color.Green === 5`, `Color[5] === "Green"`) from the reflected enumerators. Enum values convert as numbers whether or not exported.
- `error` registers `E` ([Errors](#errors)) and exports its JS class; a non-void `Base` must be registered too (for example exported earlier in the list).

### `iterable`, `indexer`

```cpp
Alka::iterable<Path>(specs...)                               // for (const p of path), [...path]
Alka::indexer()                                              // path[0], path[1] = p: size() + operator[]
Alka::indexer<&Grid::cell, &Grid::setCell, &Grid::count>()   // any accessors; nullptr keeps a default
```

- `iterable` installs `[Symbol.iterator]()`. With `size()` + `operator[]` the iterator holds the container and a cursor, bounds-checked against the live container on every step (mutation during iteration is safe); otherwise the `begin()` / `end()` elements are snapshotted into an array first.
- `indexer` makes indices readable, writable and enumerable. Accessors are member functions or callables taking the container first. Writable with a setter, or with the default getter when elements are assignable; a custom getter without setter is read-only. Out-of-range reads fall through to ordinary lookup (`undefined`); out-of-range writes are ignored, like typed arrays.
- Elements of bound-class type are copies by default (they must be copy-constructible); `policy::internalRef` gives live views (index-addressable containers only), looked up again on every use: they never dangle, a gone element throws.

### `namedProperties`, `asFunction`

```cpp
Alka::namedProperties<&Config::find, &Config::put, nullptr, &Config::erase, &Config::names>()
//                    Get            Set           Has      Remove          Keys
Alka::asFunction<&Matcher::test>()                       // matcher("text")
```

- `Get`: `std::optional<V>(const std::string&)` or `V*` (empty / null: no such property). `Set`: `(const std::string&, V)`; without it dynamic properties are read-only (assigning one throws `TypeError`, other names become ordinary properties).
- `Has`: `bool(const std::string&)` (default: Get's answer). `Remove`: `bool` or `void(const std::string&)`, for `delete`. `Keys`: a range of strings, for `Object.keys` / `for...in`.
- Only string keys are intercepted; the class's own methods and props, and expandos, win.
- `asFunction<F>`: instances become callable. Accessors and `F` are member functions or callables taking the object first.

### `disposable`, `toStringFromFormat`, `toJSON`, `equals`

```cpp
Alka::disposable<&File::close>()         // f.dispose(), f[Symbol.dispose](), `using f = ...`
Alka::disposable<nullptr, "release">()   // renamed, no hook
Alka::toStringFromFormat<Vec>()          // toString() via std::formatter<Vec>
Alka::toJSON<Vec>()                      // toJSON(): plain object of all string-named props, bases first
Alka::equals<Vec>()                      // equals(other) via operator==
```

- `disposable<Before, Name>`: `Before` runs first on the live object (if it throws, nothing is released). Then every JS wrapper of the object detaches ("no longer alive") and JS's ownership ends: the object is destroyed unless C++ shares it, once no native frame uses it anymore. A borrowed object only detaches; disposing again does nothing. `[Symbol.dispose]` is installed where the V8 has it. Not for `GcObject`.

### Specs

Trailing arguments of member factories. Header: `<Alka/Binding/Meta.hpp>`, `<Alka/Binding/Policy.hpp>`

| Spec | Used on | Effect |
|---|---|---|
| `arg("name")` | callables | names the next JS parameter (`.d.ts`, errors, `classInfo`) |
| `arg("name") = value` | single callables | default when the argument is missing or `undefined` |
| `arg("name").doc("...")` | callables | `@param` JSDoc |
| `doc("...")`, `deprecated("reason")` | any | JSDoc (`@deprecated`), `MemberView` |
| `meta<"key">(v)`, `meta("key", v)` | any | bool / integer / number / string metadata |
| `readonly` | `prop`, `staticProp` | no setter (records: `.d.ts` only) |
| `tsType<"T">` | callables, `prop`, `staticProp`, `var`, record fields | replaces the `.d.ts` type (callables: the return type) |
| `policy::borrow` / `copy` / `move` / `take` / `internalRef` | callables, props, idioms | [Return policies](#results-and-return-policies) |
| `keepAlive<I>` | methods | `this` keeps JS argument `I` alive after the call |
| `int64As<Int64Mode::X>` | any | how 64-bit integers go to JS for this declaration |
| `noFast` | callables | no V8 Fast API path |

`arg()` specs map to JS-visible parameters in order (injected parameters and a method's object parameter do not count). Defaults must convert to the parameter's stored type; only trailing defaults lower `length` and become `?` in the `.d.ts`.

## 4. Calls

Every bound callable (function, method, constructor, accessor, `callable()`) goes through one pipeline: convert arguments, call, map exceptions, convert the result. Header: `<Alka/Binding/Params.hpp>`; behavior in `<Alka/Detail/Invoke.hpp>`

### Parameters

```cpp
template<class T> struct Rest : std::vector<T> {};       // every remaining JS argument
struct CallInfo { v8::Isolate* mIsolate; v8::Local<v8::Context> mContext; v8::Local<v8::Object> mThis;
                  v8::Local<v8::Value> mNewTarget; const v8::FunctionCallbackInfo<v8::Value>* mRaw; };
Alka::fn<"log", [](std::string_view tag, Alka::Rest<Alka::Value> parts) { /* ... */ }>()
Alka::method<"spawn", [](World& w, Alka::CallInfo call, std::string name) { /* ... */ }>()
```

| Parameter | Accepts | Notes |
|---|---|---|
| `T`, `const T&` with a converter | per [Conversions](#5-conversions) | non-const `T&` only for bound classes |
| bound `T&`, `const T&` | a live wrapper of `T` or a subclass | borrowed for the call |
| bound `T*` | wrapper, or `null` / `undefined` (`nullptr`) | |
| `std::string_view` | strings only | valid for the call; short strings need no allocation |
| `const char*` | string, or `null` / `undefined` (`nullptr`) | valid for the call |
| `std::span<const T>` | typed array of `T`'s own type (bytes: any buffer or view) in place, else a converted copy | in place only when neither shared nor resizable (pinned for the call) |
| `std::span<T>` | typed array of `T`'s own type (bytes: any buffer or view), in place | not shared or resizable, else `TypeError`; writes reach JS |
| `Rest<T>` | every remaining argument | last JS-visible parameter only |
| `CallInfo`, `v8::Isolate*`, `ConvertCtx&`, `v8::Local<v8::Context>` | nothing (injected) | `CallInfo::mRaw` is the escape hatch |
| `BufferView`, `TypedView<T>`, `Shared*` | binary data, zero-copy | may outlive the call and change threads |
| `Function<Sig>`, `ThreadSafeFunction<Sig>`, `std::function<Sig>` | a function | [JS handles](#6-js-handles) |
| `std::shared_ptr<T>`, `std::weak_ptr<T>` | an owning wrapper | pinned share / observer |
| `std::unique_ptr<T>` | compile error | JS cannot give up sole ownership |
| `Retained<T>` | as `T` | the receiver becomes the owner |
| `AbortSignal` | an AbortSignal-like object | |

- A missing or `undefined` argument takes its `arg()` default, else `undefined` is converted (`std::optional<T>` becomes empty). A single callable ignores extra arguments.
- A failed conversion throws `TypeError`, e.g. `arg 1 (target) of Player.attack: expected Entity, got number`. Values never coerce (`"5"` is not an `int`). When JS ran during the conversion and threw (a getter, a proxy trap), that exception propagates unchanged instead, and ends an overload search; host-side conversions carry it in `ConvertError::mJsException` (`exceptionValue`).

### Results and return policies

| C++ result | JS value |
|---|---|
| `void`, `std::expected<void, E>` | `undefined` |
| a type with `Converter<T>::toJs` | converted (`bool`, up-to-32-bit integers, `float`, `double` set directly) |
| bound `T` by value | a new JS-owned object (moved or copied) |
| `std::unique_ptr<T>` / `std::shared_ptr<T>` | JS owns / co-owns it; empty: `null` |
| `std::weak_ptr<T>` | a weak wrapper, usable while C++ keeps the object; expired: `null` |
| `T*`, `T&` | per return policy (default: lent); `nullptr`: `null` |
| `std::expected<V, E>` | `V`, or `E` thrown ([Errors](#errors)) |
| `Task<T>` / `std::generator<T>` | a promise / an iterator ([Coroutines](#coroutines-and-generators)) |

```cpp
enum class ReturnPolicy : std::uint8_t { Automatic, Borrow, Copy, Move, Take, InternalRef };
Alka::method<&Scene::find>(Alka::policy::borrow)       Alka::method<&Node::parent>(Alka::policy::internalRef)
Alka::method<&Scene::add>(Alka::keepAlive<0>)          // the scene keeps argument 0 alive
```

| Policy for `T*` / `T&` results | Effect |
|---|---|
| none (`Automatic`) | lend; bound-class fields: `internalRef` |
| `policy::borrow` | lend; C++ keeps ownership. Also the explicit opt-out of strict borrowing |
| `policy::copy` / `policy::move` | copy / move into a new JS-owned object (`move`: non-const `T&` only) |
| `policy::take` | JS takes ownership of a `T*` and deletes it (pointer results only) |
| `policy::internalRef` | a view that keeps `this` alive |

- Lending an object that a `shared_ptr` manages (`enable_shared_from_this`) shares it instead.
- `internalRef` on a method or getter returning `T&` / `T*` makes a *live path*: every later use runs the accessor again with the same arguments, so the view never dangles (a gone part throws). Its parameters must be by value; without arguments the same path yields the same JS object. On owned or value results, the result keeps `this` alive.
- `internalRef` and `keepAlive` need a `this` (compile errors on functions and static methods). `releaseKept` drops what `keepAlive` kept ([Lifetime tools](#lifetime-tools)).

### Ownership and strict borrowing

- Identity: one wrapper per C++ object per context (keyed by the complete object's address within its class hierarchy). A later owning hand-over (by value, `unique_ptr`, `shared_ptr`, `take`) upgrades an existing borrowed wrapper in place. JS-created objects are owned by their wrapper.
- Using a wrapper whose object is gone throws `TypeError: Player.attack: the native Player object is not (or no longer) alive`.
- A `shared_ptr<T>` parameter taken from an owning wrapper is *pinned*: while C++ holds a copy, the wrapper (identity, expandos, `Retained` values, JS overrides) stays alive; `pin(isolate, ptr)` gets one explicitly. A borrowed wrapper or a live view cannot yield one (conversion error). A `weak_ptr<T>` parameter observes without pinning.
- Still undefined: destroying a borrowed object while a JS call into it runs, and C++ taking ownership of a JS-created object behind Alka's back (`shared_from_this` instead of `pin`).

Strict borrowing (`ALKA_STRICT_BORROW=1`, the default): lending JS a `T*` / `T&` of a bound class (a result, or `toJs` of a `T*`) compiles only when one of these holds. Header: `<Alka/Convert/Classes.hpp>`

- `T` derives from `Alka::Trackable`, `T` or a bound base declares a `Lifetime`, or `T` is a `GcObject`;
- the declaration states `policy::borrow`, or the value is wrapped as `Alka::borrow(ptr)`.

Classes with `enable_shared_from_this` are checked at run time instead: a `shared_ptr`-owned object is shared with JS, any other throws `JsException("Alka (strict borrowing): ...")`. Static variables bound with `staticProp` and arguments passed to JS overrides are lent without the check.

```cpp
template<class T> struct Borrowed { T* mPtr; };          // also a callback parameter type: "I vouch for it"
template<class T> constexpr Borrowed<T> borrow(T* ptr) noexcept;
```

### Errors

C++ exceptions become JS exceptions; registered types become real JS `Error` subclasses. Header: `<Alka/Runtime/Errors.hpp>`, `<Alka/Core/Error.hpp>`

```cpp
struct NotFound : std::runtime_error { using runtime_error::runtime_error; int mCode = 404; };
Alka::registerError<NotFound>("NotFound");                         // registerError<E, Base = void>(jsName)
Alka::registerError<NotFound>("NotFound", [](const NotFound& e, Alka::Object& error) {
    (void)error.set("code", e.mCode);                              // optional decorator: extra fields
});
Alka::registerError<Missing, NotFound>("Missing");                 // JS: class Missing extends NotFound
```

- An exception escaping a bound call, accessor, constructor or coroutine becomes `new NotFound(e.what())` of the most-derived registered type (`instanceof NotFound`, `instanceof Error`, a real stack); other `std::exception`s become `Error(e.what())`, anything else `Error("unknown C++ exception in <site>")`.
- A `JsException` carrying a JS exception (from `Function::call`, `await`, ...) rethrows the original JS value unchanged; otherwise `Error(message)`.
- Registration is process-wide, any thread, idempotent per type: the first registration fixes the JS name and base; a decorator sticks once given (also after an `error<>` member registered the type) and runs on the throwing isolate thread; a throwing decorator leaves the error undecorated. The message is `what()`, else `message()`, else the `typeid` name.
- `std::expected<V, E>` throws its error side: a registered class type as its JS class; `ConvertError` rethrown (with a carried JS exception); string-like `E` as `Error(text)`; `std::exception` as `Error(what())`; types with `message()` (e.g. `std::error_code`) as `Error(message())`; any other JS-convertible `E` as its converted value.
- A pending termination is never replaced by an error.

### Coroutines and generators

- `Task<T>` results become promises ([Task](#task-await-offload-resumeon)). `std::generator<T>` results (when `<generator>` exists: `ALKA_HAS_GENERATOR`) become JS iterators with `next()`, `return()` (ends early, frees the frame) and `[Symbol.iterator]`; an exception ends the generator; re-entering `next()` throws `TypeError: Generator is already running`.
- Both outlive the call, so parameters must be owned values: references, pointers, `string_view`, spans and injected parameters are compile errors. Take values, `shared_ptr<T>` and Alka handles.
- A coroutine or generator *method* keeps its receiver alive until the task settles (while the iterator lives) and needs a receiver JS owns or shares; a borrowed one throws `TypeError`.

### 64-bit integers

```cpp
enum class Int64Mode : std::uint8_t { BigInt, Number, Auto };
Alka::fn<&fileSize>(Alka::int64As<Alka::Int64Mode::Number>)
```

- Input accepts a number (an exact integer within 2^53 - 1; beyond that an error says "pass a bigint") or a bigint that fits losslessly. Integers of 32 bits and less never accept bigints.
- Output follows the declaration's `int64As<>`, else `ALKA_INT64_MODE` (default `BigInt`): `BigInt` always a bigint; `Number` always a number (rounds beyond 2^53); `Auto` a number when exactly representable, else a bigint. It covers everything the declaration converts (container elements too); host code sets `ConvertCtx::mInt64Mode`.

### V8 Fast API

When TurboFan optimizes a caller, eligible callables are called through a generated C function with identical behavior (same validation, messages and exception mapping). Header: `<Alka/Detail/FastApi.hpp>`

- Eligible: `method`, `staticMethod` and `fn` declarations with one callable, no `arg()` defaults, no policy, no `keepAlive`, no `noFast`; parameters `bool`, integers up to 32 bits (not `char`), `float`, `double`, `std::string`, `std::string_view` (by value or `const&`); result `void` or one of those numbers. Constructors and property accessors never are. Strings take the fast path only as flat one-byte strings.
- Needs `ALKA_FAST_API=1` (default when `<v8-fast-api-calls.h>` exists) and a V8 whose `FastApiCallbackOptions` carries the isolate (about 12.6+); otherwise every call is regular.
- A fast call must not call into JS: mark such callables `Alka::noFast`.

## 5. Conversions

### Built-in conversions

"From JS" is `fromJs`, "To JS" is `toJs`; TS is the `.d.ts` type (param / return where they differ). Headers: `<Alka/Convert/*.hpp>`, handles in `<Alka/Js/*.hpp>`

| C++ | From JS | To JS | TS |
|---|---|---|---|
| `bool` | boolean only | boolean | `boolean` |
| integers up to 32 bits (any integral but `bool`, `char`) | exact in-range integer number | number | `number` |
| 64-bit integers | number (exact, within 2^53 - 1) or lossless bigint | per `Int64Mode` | `number \| bigint` / per mode |
| `float`, `double` | number | number | `number` |
| enums | number naming a reflected enumerator | number | enum name |
| `std::string` | string (UTF-8; lone surrogates become U+FFFD) | string | `string` |
| `std::string_view`, `const char*` | parameters only ([Parameters](#parameters)) | string (`nullptr`: `""`) | `string` (a `const char*` parameter: `string \| null`) |
| `std::u16string` (`_view`: to JS only) | string, UTF-16 1:1 | string | `string` |
| `std::wstring` | string (UTF-16 on Windows, UTF-32 elsewhere) | string | `string` |
| `std::filesystem::path` | string (UTF-8) | string | `string` |
| `std::monostate`, `std::nullptr_t` | `undefined` or `null` | `undefined`, `null` | `undefined`, `null` |
| `std::optional<T>` | `undefined` / `null`: empty | empty: `undefined` | `T`, optional |
| `std::vector<T>`, `std::deque<T>`, `std::list<T>` | array; numeric `T` also any typed array (checked per element) | array | `T[]` (numeric param: `T[] \| Float64Array` etc.) |
| `std::array<T, N>` | array of length N | array | `T[]` |
| `std::vector<std::uint8_t>` / `<std::byte>`, `std::span<const std::uint8_t / std::byte>` | ArrayBuffer or any view, copied (a span parameter uses it in place: [Parameters](#parameters)) | `Uint8Array` (copy) | `ArrayBuffer \| ArrayBufferView` / `Uint8Array` |
| `std::map`, `std::unordered_map` with `std::string` keys | plain object (own enumerable string keys) | plain object | `Record<string, V>` |
| `std::map`, `std::unordered_map`, other keys | `Map` | `Map` | `Map<K, V>` |
| `std::set`, `std::unordered_set` | `Set` | `Set` | `Set<T>` |
| `std::pair`, `std::tuple` | array of exactly that length | array | `[A, B]` |
| `std::variant<Ts...>` | first alternative, in order, that converts | the held alternative | `A \| B` |
| `std::chrono::duration` | finite number of milliseconds (exact for integral reps) | milliseconds | `number` |
| `system_clock::time_point` | valid `Date`, or ms since the epoch | `Date` | `Date \| number` / `Date` |
| `std::function<R(Args...)>` | function (held strongly) | a new JS function | `(arg0: A) => R` |
| bound class `T` (copyable) | wrapper: a copy | a new JS-owned copy | `T` |
| bound `T*`; `Borrowed<T>` | wrapper, or `null` / `undefined`; `Borrowed`: none | lent wrapper (`Borrowed`: no strict check), `nullptr`: `null` | `T \| null` |
| `std::shared_ptr<T>` | owning wrapper: pinned share; `null` / `undefined`: empty | JS co-owns; empty: `null` | `T \| null` |
| `std::weak_ptr<T>` | owning wrapper: observer | weak wrapper; expired: `null` | `T \| null` |
| `std::unique_ptr<T>` | compile error | JS owns (host code moves it in: `toJs(cx, std::move(p))`); empty: `null` | `T \| null` |
| records | plain object, field by field | plain object | interface name |
| `Value` / `Object` / `Array` | any / objects / arrays | the value (empty: `undefined` / `null` / `null`) | `unknown` / `object` / `unknown[]` |
| `Function<R(Args...)>`, `ThreadSafeFunction<R(Args...)>` | function | the function (TSF: from JS only) | `(arg0: A) => R` |
| `Promise<T>`, `Task<T>` | none | the promise | `Promise<T>` |
| `Retained<T>` | as `T`, owner = receiver | the value | as `T` |
| `NativeFunction<F>` (`callable`) | none | a new JS function | its JS signature |
| `AbortSignal` | object with `aborted` and `addEventListener` | none | `AbortSignal` |
| `BufferView`, `SharedBufferView` | ArrayBuffer or view, zero-copy (Shared: also SharedArrayBuffer) | `Uint8Array` over the same memory | `ArrayBuffer \| ArrayBufferView` (`\| SharedArrayBuffer`) / `Uint8Array` |
| `TypedView<T>`, `SharedTypedView<T>` | typed array of `T`'s own type, zero-copy | typed array over the same memory | e.g. `Float32Array` |
| `JsIterator<E>` | none | the iterator | `IterableIterator<E>` |
| `cppgc::Member<U>`, `WeakMember<U>`, `Persistent<U>`, `WeakPersistent<U>` | wrapper or `null` / `undefined` | the object's wrapper or `null` | `U \| null` |

- `char` has no converter. `std::optional<T>` parameters render as `name?: T` when trailing, otherwise `T | undefined`; optional results and props as `T | undefined`.
- Nested errors name their location: `element 2: ...`, `key 'x': ...`.

### Custom conversions: `Converter<T>`, `TsTypeName<T>`

Header: `<Alka/Convert/Converter.hpp>`, `<Alka/Ts/TsTypes.hpp>`

```cpp
template<> struct Alka::Converter<Rgb> {                  // either side may be omitted
    static Alka::Result<Rgb> fromJs(Alka::ConvertCtx& cx, v8::Local<v8::Value> value) {
        auto text = Alka::fromJs<std::string>(cx, value);   // Result<std::string>
        if (!text) return std::unexpected(Alka::ConvertError::typeMismatch("color", Alka::describeJsValue(value)));
        return parseRgb(*text);
    }
    static v8::Local<v8::Value> toJs(Alka::ConvertCtx& cx, const Rgb& c) { return Alka::toJs(cx, c.hex()); }
};
template<> struct Alka::TsTypeName<Rgb> { static Alka::TsType get(const Alka::TsNameRegistry&) { return {"string"}; } };
```

- `fromJs` must return exactly `Result<T>`, `toJs` exactly `v8::Local<v8::Value>`; the concepts `FromJsConvertible<T>`, `ToJsConvertible<T>` and `JsConvertible<T>` detect them. Constrained partial specializations work (the built-ins use them). Declare a specialization before its first use.
- Free functions: `fromJs<T>(cx, value) -> Result<T>`, `toJs(cx, value) -> v8::Local<v8::Value>`, `describeJsValue(value)` ("number", "Float64Array", "null", ...). A converter may throw; inside a bound call that becomes a JS exception.
- `TsType{mText, mOptionalLike}`; `TsNameRegistry::mPosition` (`Param` / `Return`) and `mInt64Mode` give the direction and mode; `reg.refer(typeid(X), fallback)` names a registered type.

### `ConvertCtx`, `Result<T>`, `ConvertError`, `JsException`

Header: `<Alka/Convert/Converter.hpp>`, `<Alka/Core/Error.hpp>`

```cpp
struct ConvertCtx {                                // host code: Alka::ConvertCtx cx{isolate, context};
    v8::Isolate* mIsolate; v8::Local<v8::Context> mContext;
    Int64Mode mInt64Mode = kDefaultInt64Mode;      // how 64-bit integers go to JS
    v8::Local<v8::Object> mReceiver;               // `this` of the bound member call, if any
};
template<class T> using Result = std::expected<T, ConvertError>;
struct ConvertError {                              // mMessage; mJsException: the thrown JS value, if any
    const std::string& message() const&;           // factories: typeMismatch(expected, got), outOfRange(expected, got),
};                                                 //   forArgument(e, index, arg, fn), forElement(e, i), forKey(e, key)
class JsException : public std::exception {        // JsException(std::string message), JsException(ConvertError)
    const char* what() const noexcept;  const std::string& message() const noexcept;
};
```

- Bound calls build the context per call, with the declaration's int64 mode and receiver.
- `exceptionValue(error)` returns the JS value a `ConvertError` or `JsException` carries.

## 6. JS handles

### Thread rules

- The *isolate thread* is the current thread when the isolate is entered on it, when it holds the isolate's `v8::Locker`, or (for hosts that never use Lockers) when it is the thread Alka first saw the isolate on.
- Off that thread handle operations fail with an error ("Alka handle used off its isolate's thread") instead of touching V8. Dropping the last copy anywhere is fine: the release is handed to the isolate thread. After `disposeIsolate`, handles report "the handle's isolate was disposed" instead of dangling.
- A strong handle keeps its value and creation context alive; operations run in that context, inside their own HandleScope. Converting a handle into another isolate throws `JsException`.
- Operations return `Result`s (JS exceptions arrive as the error's `mJsException`); `Function::call` (and the `std::function` built on it) and `await` throw `JsException` instead.

### `Value`

Header: `<Alka/Js/Value.hpp>`

```cpp
class Value {                                         // default: empty
public:
    Value(ConvertCtx& cx, v8::Local<v8::Value> value);
    bool valid() const noexcept;  explicit operator bool() const noexcept;
    v8::Isolate* isolate() const noexcept;  bool belongsTo(const v8::Isolate*) const noexcept;
    v8::Local<v8::Value> local(v8::Isolate*) const;   // isolate thread, in a HandleScope; creationContext(isolate)
    template<FromJsConvertible T> Result<T> to() const;
    JsType type() const;  std::string_view typeOf() const;  bool strictEquals(const Value& other) const;   // ===
    Result<std::string> toString() const;  Result<std::string> toJson(int indent = 0) const;
    template<class R = Value, class... A> Result<R> tryCall(const A&... args) const;          // this: undefined
    template<class R = Value, class... A> Result<R> tryCallOn(const Value& self, const A&... args) const;
    template<class... A> Result<Object> tryConstruct(const A&... args) const;                // new value(...)
};
enum class JsType : std::uint8_t { Invalid, Undefined, Null, Boolean, Number, BigInt, String, Symbol,
                                   Function, Array, Promise, Object };
Result<Value> parseJson(ConvertCtx& cx, std::string_view text);
Value exceptionValue(const ConvertError&);    Value exceptionValue(const JsException&);
```

- Predicates: `isUndefined`, `isNull`, `isNullish`, `isBoolean`, `isNumber`, `isBigInt`, `isString`, `isSymbol`, `isFunction`, `isArray`, `isPromise`, `isObject` (any object, functions included).
- `type()` is `Invalid` (and `typeOf()` `""`) for an empty, disposed or off-thread handle. `toString` is JS `String(v)`; `toJson` is `JSON.stringify` and fails for `undefined`, functions and symbols.

### `Object`, `Array`, `WeakValue`

Header: `<Alka/Js/Value.hpp>`

```cpp
class Object : public Value {
    static Object create(ConvertCtx& cx);  static Object global(ConvertCtx& cx);
    Result<bool> has(std::string_view key) const;      // key in obj; hasOwn(key): Object.hasOwn
    Result<bool> remove(std::string_view key) const;   // delete obj[key]
    Result<std::vector<std::string>> keys() const;     // Object.keys
    Result<Value> get(std::string_view key) const;     // getAs<T>(key) -> Result<T>: converted
    template<class T> Result<void> set(std::string_view key, const T& value) const;
    template<class R = Value, class... A> Result<R> tryCallMethod(std::string_view name, const A&... args) const;
};
class Array : public Object {                          // also get / getAs / set by index, push(value)
    static Array create(ConvertCtx& cx, std::size_t length = 0);    // holes
    Result<std::size_t> size() const;
};
template<class T = Value> class WeakValue {             // T: Value, Object or Function<...>
    explicit WeakValue(const T& value);                 // isolate thread
    bool expired() const noexcept;  T lock() const;     // lock: isolate thread; empty once collected
};
```

### `Function`, `callable`

Calling JS from C++, and C++ callables made into JS functions at run time. Header: `<Alka/Js/Function.hpp>`, `<Alka/Convert/Functional.hpp>`

```cpp
template<class R, class... Args> class Function<R(Args...)> : public Value {
    Result<R> tryCall(Args... args) const;                          // this: undefined
    Result<R> tryCallOn(const Value& receiver, Args... args) const;
    R call(Args... args) const;                                     // throws JsException
};
template<class F> NativeFunction<std::decay_t<F>> callable(F fn, std::string_view name = "native function");
(void)Alka::Object::global(cx).set("now", Alka::callable([&clock] { return clock.now(); }, "now"));
```

- `Function`: arguments need `toJs`; `R` needs `fromJs` (or is `void`); a return value that does not convert is an error. A `JsException` from `call` escaping a bound callable rethrows the original JS exception object. Isolate thread only; use `ThreadSafeFunction` elsewhere.
- `callable`: any callable with exactly one call operator (stateful and move-only ones too, no generic lambdas). JS calls it like a bound function (conversions, `Rest<T>`, injected parameters, `std::expected`, `Task`; no defaults). All JS functions made from one `NativeFunction` share the callable, which lives until the last of them is collected: what it captures by reference must outlive that.
- `std::function<R(Args...)>` converts to a new JS function the same way. From JS it wraps a `Function` held strongly (released on the isolate thread); calling it throws `JsException` on failure, off-thread included.

### `Promise`, `Settler`

A JS promise settled from C++. Header: `<Alka/Js/Promise.hpp>`

```cpp
template<class T = void> class Promise : public Value {
    explicit Promise(ConvertCtx& cx);
    static Promise create();                               // entered isolate + context; else empty
    Result<void> resolve(const T& value) const;            // Promise<void>: resolve()
    Result<void> reject(std::string_view message) const;   // new Error(message); or reject(const Value&)
    Settler<T> settler() const;                            // create on the isolate thread
    v8::Local<v8::Promise> promise(v8::Isolate*) const;
};
template<class T = void> class Settler {                   // copyable; any thread
    bool resolve(T value) const;  bool reject(std::string message) const;  bool settled() const noexcept;
};
```

- Create it inside a bound call (or with a context entered), return it, settle it later on the isolate thread; reactions run at the next microtask checkpoint. `Promise<T>` converts to JS only: receive JS promises as `Value` and `co_await` them.
- `Settler`: the outcome travels through the isolate's `Dispatcher` and lands at the host's next `drain()`. Only the first resolve / reject counts; false once settled or disposed. A pending settler keeps `Dispatcher::busy()` true until it settles or its last copy is gone.

### `Task`, `await`, `offload`, `resumeOn`

C++20 coroutines as JS promises. Header: `<Alka/Js/Task.hpp>`

```cpp
Alka::Task<std::string> fetchTitle(std::string url) {
    auto page = co_await Alka::offload([url] { return httpGet(url); });     // worker thread
    auto parsed = co_await Alka::await<Alka::Object>(jsParser.call(page));  // jsParser: Function<Value(std::string)>
    co_return parsed.getAs<std::string>("title").value();
}
Alka::fn<&fetchTitle>()                                // fetchTitle(url: string): Promise<string>
template<class U = Value> auto await(Value value);     // co_await yields U; throws JsException on rejection
template<class U = Value> auto tryAwait(Value value);  // co_await yields Result<U>
template<class F> auto offload(F fn);                  // fn on a new thread
auto resumeOn(const Dispatcher& dispatcher);           // back to the dispatcher's isolate thread
```

- A `Task` starts eagerly inside the bound call; `co_return` resolves its promise, an escaping exception rejects it (JS exceptions keep their object, registered error types their class).
- It resumes on the isolate thread, in the context it was suspended in: after a JS promise settles (microtasks) or, after `offload` / `resumeOn`, at the host's next `Dispatcher::drain()`. `await` of a non-promise is ready at once; `co_await task` awaits another task's promise.
- `offload(fn)`: the frame stays alive, even through isolate disposal, until `fn` returns, so `fn` may use the coroutine's locals by reference; the dispatcher stays `busy()` meanwhile.
- `resumeOn`: immediate when already on that thread; if the isolate is disposed first the coroutine is destroyed. Create the `Dispatcher` on the isolate thread.
- Frames awaiting JS are destroyed at isolate disposal; a coroutine finishing on another thread settles its promise at the next drain. Parameter rules: [Coroutines](#coroutines-and-generators).

### `Retained`

A JS value held by a C++ object without an uncollectable cycle. Header: `<Alka/Js/Retained.hpp>`

```cpp
struct Button { Alka::Retained<Alka::Function<void()>> mOnClick; };
Alka::prop<"onClick", &Button::mOnClick>()       // JS: button.onClick = () => button.x++
template<class T = Value> class Retained {       // move-only
    template<class Owner> void set(Owner* owner, const T& value);    // from C++: mOnClick.set(this, f)
    void set(ConvertCtx& cx, v8::Local<v8::Object> owner, v8::Local<v8::Value> value);
    void reset();  T get() const;                // get: strong handle, or empty
    explicit operator bool() const noexcept;  bool weak() const noexcept;   // weak: held via the owner (cycle-safe)
};
```

- While JS solely owns the object, the value is stored on its JS wrapper and only weakly referenced from C++, so `closure -> wrapper -> C++ -> closure` cycles are collectable.
- Otherwise (borrowed objects, objects C++ co-owns) it is held strongly and turns weak once JS comes to own the object (an owning hand-over, or the next `get()` after C++ let go). Pinned shares keep the wrapper and its values alive.
- As a property or parameter the receiver is the owner. Isolate thread; off it `reset()` releases only the C++ side.

### `ThreadSafeFunction`

A JS function C++ may call from any thread. Header: `<Alka/Js/ThreadSafeFunction.hpp>`

```cpp
void watch(std::string path, Alka::ThreadSafeFunction<void(std::string)> onLine);   // as a parameter
template<class R, class... Args> class ThreadSafeFunction<R(Args...)> {
    using ErrorHandler = std::function<void(const ConvertError&)>;
    explicit ThreadSafeFunction(Function<R(Args...)> fn, ErrorHandler onError = {});    // isolate thread
    bool valid() const noexcept;
    bool post(Args... args) const;                      // any thread; false once the isolate is gone
    std::future<Result<R>> call(Args... args) const;    // any thread; never wait on it on the isolate thread
    void keepAlive(bool on) const;                      // hold the host loop (Dispatcher::busy)
};
```

- Each call is queued to the isolate's `Dispatcher` and runs there in order at the next drain; arguments are copied when queued and converted on the isolate thread. A posted call that throws reports to `onError` (isolate thread).
- Holding one does not keep the host loop busy unless `keepAlive(true)` (like N-API refs).

### `AbortSignal`

A JS AbortSignal as a C++ `std::stop_token`. Header: `<Alka/Js/Abort.hpp>`

```cpp
Alka::Task<std::string> fetchText(std::string url, Alka::AbortSignal signal) {
    std::stop_callback cancel(signal.token(), [&] { request.cancel(); });
    co_await Alka::offload([&] { request.run(); });
    signal.throwIfAborted();                     // rejects with the JS reason
    co_return request.text();
}
class AbortSignal {                              // default-constructed: never aborts
    std::stop_token token() const noexcept;  bool aborted() const noexcept;    // any thread
    void throwIfAborted() const;                 // JsException carrying signal.reason
};
```

- Accepts any object with `aborted` and `addEventListener('abort', ...)` (browsers, Node, the Engine globals); an already aborted signal converts as aborted. The listener holds the C++ state weakly.

### Binary data views

Zero-copy access to JS binary memory. Header: `<Alka/Convert/Buffers.hpp>`

```cpp
void hash(Alka::BufferView bytes);                         // JS: hash(new Uint8Array(...))
void normalize(Alka::TypedView<float> v);                  // JS: normalize(new Float32Array(...))
Alka::BufferView render(v8::Isolate* isolate) {            // isolate injected
    auto out = Alka::BufferView::allocate(isolate, 4096);  // JS gets a Uint8Array over this memory
    fill(out.bytes());
    return out;
}
class BufferView {        // bytes() -> std::span<std::byte>; data(), size(), empty(), offset(), backingStore()
    static BufferView allocate(v8::Isolate*, std::size_t length);
};
template<class T> class TypedView {   // span() -> std::span<T>; data(), size(), [], begin(), end(), byteOffset()
    static TypedView allocate(v8::Isolate*, std::size_t length);   // zero-filled elements
};
class SharedBufferView : public BufferView { /* allocate() */ };
template<class T> class SharedTypedView : public TypedView<T> { /* allocate() */ };
```

- A view pins the backing store: it stays valid after JS drops, detaches or transfers the buffer, and may go to other threads (concurrent JS writes are the caller's concern).
- `BufferView` / `TypedView` refuse SharedArrayBuffer memory and resizable ArrayBuffers. The `Shared*` variants accept shared memory (racy: copy out or use atomics); resizable ArrayBuffers stay refused, growable SharedArrayBuffers are fine.
- `TypedView<T>` needs a typed array of `T`'s own type: `int8_t`, `int16_t` / `uint16_t`, `int32_t` / `uint32_t`, `int64_t` (BigInt64Array), `uint64_t` (BigUint64Array), `float`, `double`; bytes (`uint8_t`, `std::byte`) use `BufferView`.
- `allocate`: `BufferView` / `TypedView` on any thread that may allocate V8 backing stores (the isolate's); the zero-filled `Shared*` ones on the isolate thread.

## 7. Runtime

### `install`, `installInto`, `registerClass`

Materializes bindings. Header: `<Alka/Runtime/Install.hpp>`

```cpp
template<BoundModule... Mods> void install(v8::Local<v8::Context> context);
template<BoundModule Mod> void installInto(v8::Local<v8::Context> context, v8::Local<v8::Object> target);
template<BoundClass... Ts> void registerClass(v8::Isolate* isolate);
```

- Isolate thread, isolate entered. `install` defines `Mods` and their submodules on the global object; `installInto` defines `Mod`'s members on `target` (e.g. a Node addon's `exports`), submodules as nested objects named by their last path segment.
- A bound object converts to JS only once its class or a bound base has a template in the isolate (`install`, an import of a registered module, or `registerClass`), else `JsException: class X (or any of its bases) is not installed in this isolate`. `registerClass` creates templates (bases included) without installing anything, for hosts that only mint instances from C++; idempotent.

### ES modules

Bound modules as real ES modules (the runtime twin of `TsStyle::EsModule`). Header: `<Alka/Runtime/Modules.hpp>`

```cpp
template<BoundModule... Mods> void registerModules(v8::Local<v8::Context> context);
v8::MaybeLocal<v8::Module> resolveModule(v8::Local<v8::Context>, v8::Local<v8::String> specifier);
v8::MaybeLocal<v8::Module> moduleResolveCallback(v8::Local<v8::Context>, v8::Local<v8::String> specifier,
                                                 v8::Local<v8::FixedArray> attributes, v8::Local<v8::Module> referrer);
```

- Each module, and each submodule under its own path, becomes a `SyntheticModule` exporting its functions, classes, enums, error classes and vars. Classes are the per-context constructors: `import { Player } from "game"` is the same function as a global `game.Player`. Independent of `install`: use either or both.
- `resolveModule` returns the module registered for `specifier` (created on first use, reused while referenced) or empty, and throws nothing; hook it into the host's resolver. `moduleResolveCallback` is a ready `ResolveModuleCallback` that throws "Cannot find module '...'" for anything else.

### `Dispatcher`

Gets work onto an isolate's thread from anywhere. Header: `<Alka/Runtime/Dispatcher.hpp>`

```cpp
class Dispatcher {
    explicit Dispatcher(v8::Isolate* isolate);              // isolate thread; copies go anywhere
    template<class F> bool post(F&& task) const;            // any thread; false once disposed
    std::size_t drain(std::size_t max = SIZE_MAX) const;    // isolate thread; returns tasks run
    void setWakeup(std::function<void()> wakeup) const;     // posting thread, when work arrives while idle
    bool busy() const;  void setActivityHook(std::function<void()> hook) const;   // hook: in-flight work starts / ends
    void retain() const noexcept;  void release() const noexcept;   // external in-flight work
    bool alive() const noexcept;   explicit operator bool() const noexcept;
};
```

- `drain()` runs Alka's deferred maintenance, up to `max` queued tasks (FIFO, each in its own HandleScope), then deferred destructions; it does nothing off the isolate thread.
- Posted tasks run only when the host drains, never inside a bound call. `Settler`, `offload`, `resumeOn`, `ThreadSafeFunction` and cross-thread `Task` completions all use this queue, so a host must drain it (`Engine::pump` and `Node::attach` do).
- `busy()` covers offloaded steps, pending settlers, kept-alive thread-safe functions, `retain()`s and queued tasks: keep an "exit when idle" loop running while true. Pair every `retain()` with a `release()`. The hooks replace previous ones; the activity hook must not throw.

### Isolate disposal

Header: `<Alka/Detail/State.hpp>` (included by the runtime headers)

```cpp
void disposeIsolate(v8::Isolate* isolate);                                  // before Isolate::Dispose
void onDisposeIsolate(v8::Isolate* isolate, std::function<void()> hook);    // isolate thread
```

- Releases everything Alka holds for the isolate: wrappers (native objects JS still owns are destroyed), templates and handles; drops queued tasks and destroys suspended coroutine frames. Required because V8 runs no teardown weak callbacks. `IsolateHandle` (Engine) and `Node::attach` call it for you.
- Hooks run first, latest registered first, while the isolate and every handle still work.

### Lifetime tools

Header: `<Alka/Runtime/Lifetime.hpp>`, `<Alka/Runtime/Trackable.hpp>`

```cpp
class Trackable;                                                   // base class
template<class T> void invalidate(v8::Isolate* isolate, T* ptr);
template<class T> std::shared_ptr<T> pin(v8::Isolate* isolate, T* ptr);
template<class T> void releaseKept(v8::Isolate* isolate, T* ptr);    void releaseKept(const CallInfo& call);
template<class T> void updateExternalSize(v8::Isolate* isolate, const T* ptr);
std::int64_t reportedExternalMemory(v8::Isolate* isolate);
enum class DestructionMode : std::uint8_t { Immediate, Deferred };
void setDestructionMode(v8::Isolate* isolate, DestructionMode mode);
std::size_t runDeferredDestruction(v8::Isolate* isolate, std::size_t max = SIZE_MAX);
```

- `Trackable`: for C++-owned types lent to JS. Destroying the object, on any thread, detaches every borrowed wrapper; a new object at a reused address gets a fresh wrapper. Copies get their own liveness flag.
- `invalidate`: the same, explicitly (for `Lifetime::Manual` or types you cannot change): every borrowed wrapper of `*ptr` in every context is detached; owning wrappers are left alone. Call it before destruction, or first thing in the most-derived destructor.
- `pin`: a `shared_ptr` to a JS-owned object that keeps its wrapper alive while any copy lives (what a `shared_ptr<T>` parameter receives); null when JS does not own `*ptr`. Use it instead of `shared_from_this()` on JS-created objects.
- `releaseKept` drops what `keepAlive<I>` made the object's wrappers (or the running call's receiver) keep. `updateExternalSize` re-reports a JS-owned object's `externalSize`; `reportedExternalMemory` is the total.
- `DestructionMode::Immediate` (default): objects whose wrappers were collected are destroyed inside V8's GC callbacks (destructors must not touch JS or V8). `Deferred`: queued until `runDeferredDestruction` (also run by `Dispatcher::drain`), so destructors may do anything; switching back to `Immediate` destroys the queue.
- All on the isolate thread. The bound-class requirement is checked in the bodies, so they work in inline member functions written before the `Bind<T>` specialization.

### Snapshots

V8 startup snapshots of contexts with Alka bindings (self-hosted embedders). Not in the umbrella header: `<Alka/Runtime/Snapshot.hpp>`

```cpp
params.external_references = Alka::snapshot::externalReferences<Game>();       // build time
v8::SnapshotCreator creator(params);
// ... enter the isolate and a context, Alka::install<Game>(context), run setup scripts ...
auto templates = Alka::snapshot::addTemplates<Game>(creator);
creator.SetDefaultContext(context, Alka::snapshot::internalFieldsSerializer());
Alka::snapshot::prepare(creator.GetIsolate());                                 // releases Alka's handles
auto blob = creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
Alka::snapshot::restoreTemplates<Game>(isolate, templates);                    // start-up, same references
auto context = v8::Context::New(isolate, nullptr, {}, {}, Alka::snapshot::internalFieldsDeserializer());
```

- `externalReferences<Mods...>()` is the null-terminated table every isolate created from or for the snapshot needs. `restoreTemplates` takes the same `Mods` in the same order, before any binding is used. `prepare` goes after `SetDefaultContext` / `AddContext` / `addTemplates`, right before `CreateBlob`.
- Bound functions and classes come back working (the same class objects, so `instanceof` holds). Native objects do not travel: wrappers in the snapshot come back detached. Not covered: functions made at run time from C++ callables (`std::function` results, `callable()`), iterator objects, synthetic ES modules.

### `Watchdog`

A time limit for JS. Header: `<Alka/Runtime/Watchdog.hpp>`

```cpp
{   Alka::Watchdog watchdog(isolate, std::chrono::milliseconds(500));
    auto result = script->Run(context);           // empty when terminated
    if (watchdog.fired()) { /* timed out */ } }
```

- Terminates the isolate's running JS from a watchdog thread once the limit passes. Destroy it on the isolate thread after the guarded code returned: a termination that fired is then cancelled.

### Introspection

Header: `<Alka/Binding/Introspect.hpp>`, `<Alka/Binding/Bind.hpp>`, `<Alka/Core/Traits.hpp>`

```cpp
template<Bound T> ClassView classInfo();         // classes, records and modules
struct ClassView { std::string_view mName, mDoc, mExtends; std::vector<MemberView> mMembers;
                   const MemberView* member(std::string_view name) const noexcept; };
struct MemberView { MemberKind mKind; std::string_view mName, mDoc; bool mReadonly, mDeprecated;
                    std::string_view mDeprecatedReason; std::size_t mOverloadCount;
                    std::vector<std::string_view> mArgNames; std::vector<std::pair<std::string_view, MetaValue>> mMeta;
                    const MetaValue* meta(std::string_view key) const noexcept; };
struct MetaValue { enum class Kind : std::uint8_t { Bool, Int, Number, String } mKind;
                   bool mBool; long long mInt; double mNumber; std::string_view mString; };
```

- `MemberKind`: `Ctor`, `Method`, `StaticMethod`, `Property`, `Function`, `Var`, `EnumType`, `Submodule`, `ClassRef`, `Indexer`, `ErrorType`, `Disposer`, `StaticProperty`, `NamedProperties`, `CallAsFunction`; `type<>` and `submodule<>` entries are not listed by `classInfo`.
- Compile time: `boundNameOf<T>()`, `boundDocOf<T>()`, `modulePathOf<Mod>()`, `forEachMember<T>(f)`, `forEachMemberIndexed<T>(f)` (`f.template operator()<I>(desc)`), `MembersTupleOf<T>`, `memberCount<T>`, `ExtendsOf<T>`, `HasTrampoline<T>`, `validateClassBinding<T>()`.
- Traits: `FnTraitsOf<F>`, `FnTraitsOfType<D>`, `CallableTraitsOf<F>` (`Ret`, `Class`, `ArgsTuple`, `isMemberFunction`, `isConst`, `arity`), `isMemberCallable<F>`, `BindableCallableValue<T>`, `Enum<T>`.
- `const char* versionString() noexcept` (`<Alka/Config.hpp>`) returns `ALKA_VERSION_STRING`.

## 8. C++-heap classes (cppgc)

Bound classes living on V8's C++ heap, traced together with JS. Header: `<Alka/Binding/Gc.hpp>`

```cpp
class Node : public Alka::GcObject<Node> {           // GcObject<T>: the first base
public:
    void Trace(cppgc::Visitor* visitor) const override {
        GcObject::Trace(visitor);                    // its wrapper
        visitor->Trace(mNext);
    }
    cppgc::Member<Node> mNext;                       // JS sees a Node or null
};
template<> struct Alka::Bind<Node> : Alka::Class<Node> {
    static constexpr auto members = Alka::members(Alka::ctor<>(), Alka::prop<"next", &Node::mNext>());
};
Node* node = Alka::makeGc<Node>();                   // on the current isolate's CppHeap
cppgc::Persistent<Node> keep(node);                  // hold across calls
```

- Available when `ALKA_HAS_CPPGC` is 1: V8 13+ (`Object::Wrap`); on Windows with a V8 DLL only with `USING_V8_SHARED` (as in a Node addon); with a static V8 on Windows define `ALKA_HAS_CPPGC=1`.
- Against a V8 built by GCC (Linux Node releases, distributions' libnode), code built by clang 17+ must define `COMPONENT_BUILD`: V8's headers declare cppgc's slow paths `preserve_most`, which GCC does not implement, and the first allocation crashes otherwise. The define only drops that convention, so it is safe with any V8.
- The isolate needs a CppHeap: `IsolateOptions::mCppHeap`, `CreateParams::cpp_heap`, or Node's own; `makeGc<T>(args...)` throws `JsException` without one. `new Node()` in JS allocates there too.
- JS and C++ references between such objects form one graph: cycles through either side are collected, with no weak callbacks or ownership bookkeeping. The wrapper keeps the object alive and the object, while reachable, its one wrapper.
- They travel as `T*` / `T&` (always lendable) and as `cppgc::Member<T>`, `WeakMember<T>`, `Persistent<T>`, `WeakPersistent<T>` (empty or collected: `null`). Refused at compile time: by value, `std::shared_ptr`, `std::weak_ptr`, `std::unique_ptr`, `policy::take` / `copy` on pointer results, `policy::copy` / `move` on reference results, a `Trampoline`, `disposable`.
- With MSVC and `USING_V8_SHARED`, a `v8::SourceLocation::Current()` import missing from clang-cl-built import libraries (Node 23+, Electron) resolves to an equivalent inside Alka.

## 9. TypeScript generation

Generates a `.d.ts` from the same bindings. Header: `<Alka/Ts/Generator.hpp>`, `<Alka/Ts/TsTypes.hpp>`

```cpp
enum class TsStyle { GlobalNamespace, EsModule };
struct TsGenOptions {
    TsStyle mStyle = TsStyle::GlobalNamespace;
    std::string mBanner = "// AUTO-GENERATED by Alka " ALKA_VERSION_STRING ". Do not edit.";
    std::map<std::string, std::function<std::string(const MetaValue&)>, std::less<>> mMetaRenderers{};
    bool mDisposableSymbol = true;      // also declare [Symbol.dispose]() (TS 5.2+, lib esnext.disposable)
};
template<BoundModule... Mods> std::string generateDts(const TsGenOptions& options = {});
```

- `GlobalNamespace`: `declare namespace game { ... }` (dotted paths as is); `EsModule`: `declare module "game" { export ... }`. Submodules get their own blocks. Cross-module references are qualified (`game.Player` or `import("game").Player`); export every class a signature mentions with `type<T>`, otherwise its bare name is used.
- Classes: `class Player extends Entity`, `constructor(...)`, one line per overload, `static` methods, `readonly` for non-writable props, class `var`s as `static readonly`, `[index: number]: T` for indexers, `[name: string]: unknown` for named properties, `dispose(): void` (plus `[Symbol.dispose](): void`), and a merged `interface` holding the call signature for `asFunction`.
- Records: `interface`; enums: `enum Color { Red = 0, ... }`; errors: `class NotFound extends Error {}`; functions: `function`; vars: `const` with a literal type for numbers, bools and strings.
- Parameters are named by `arg()` (else `arg0`, ...); trailing defaults and optional-like types become `?:`; `Rest<T>` is `...name: T[]`; injected parameters do not appear.
- JSDoc from `doc`, `arg().doc`, defaults (`(default 1)`), `@deprecated`, and `meta` as `@alka key value` unless `mMetaRenderers` has a renderer for the key. `@@` members appear as `[Symbol.iterator]`; registry symbols are emitted commented out.
- Types map as in [Built-in conversions](#built-in-conversions); also `std::expected<V, E>` as `V`, `std::generator<T>` as `IterableIterator<T>`, a writable `std::span<T>` as its typed array (bytes: `ArrayBuffer | ArrayBufferView`). A type without a mapping is a compile error: specialize `TsTypeName<T>` or override the declaration with `tsType<"...">`.
- Signatures come from the callables' types, so generating a `.d.ts` never odr-uses the bound functions.

## 10. Engine layer

Optional self-hosting (CMake `ALKA_BUILD_ENGINE`, needs libplatform): V8 start-up, isolates, contexts with runtime globals, ES modules from disk, resource limits. Header: `<Alka/Engine/Engine.hpp>`

```cpp
auto isolate = Alka::Engine::createIsolate({.mMaxHeapBytes = 64 << 20});   // starts V8 on first use
Alka::ContextScope scope(isolate.get());
scope.install<Game>();
auto result = scope.runModuleFile("main.mjs", {.mTimeout = std::chrono::seconds(5)});
bool clean = Alka::Engine::runUntilIdle(isolate.get());                     // timers, promises, coroutines
```

### `Engine`, isolates

```cpp
struct EngineOptions {
    std::string mIcuDataPath{};           // empty: icudtl.dat next to the executable
    int mThreadPoolSize = 0;              // 0: platform default
    std::vector<std::string> mFlags{};    // e.g. "--expose-gc"
    v8::Platform* mPlatform = nullptr;    // a V8 the host started (InitializePlatform + Initialize done)
    bool mDefaultPlatform = true;         // mPlatform is libplatform's: pump() runs its tasks
};
struct IsolateOptions {
    std::size_t mMaxHeapBytes = 0;        // 0: V8's default
    ErrorReporter mOnError{};             // std::function<void(std::string_view)>: nobody's errors; default stderr
    bool mCppHeap = false;                // a CppHeap for GcObject classes (V8 13+)
};
class Engine {
    static Engine& instance(const EngineOptions& options = {});    // later calls ignore their options
    static IsolateHandle createIsolate(const IsolateOptions& options = {});
    static bool pump(v8::Isolate* isolate);  static bool runUntilIdle(v8::Isolate* isolate);
};
class IsolateHandle;     // move-only; owns the isolate and its allocator: get(), operator->, operator bool
```

- V8 cannot start twice: a host that started it passes `mPlatform`, and the Engine leaves it running (the other option fields are then unused).
- `createIsolate`: the isolate's Dispatcher wakes the platform's message loop, so `pump()` runs posted work. A script reaching `mMaxHeapBytes` is terminated (its eval reports `JS heap limit reached (N MB)`) instead of V8 aborting the process. `mCppHeap` without cppgc support throws `JsException`. Destroying the handle calls `disposeIsolate` and disposes the isolate.
- `pump` (isolate thread): runs everything runnable now (due timers once, platform tasks, Dispatcher drains, microtasks, unhandled-rejection reports) until nothing is left; returns whether any work ran.
- `runUntilIdle` (isolate thread): pumps until no posted or in-flight work (`Dispatcher::busy`) and no timers remain, waiting without spinning in between. Returns false when an error nobody handled was reported since the last call (unhandled rejection, throwing timer callback or microtask, `reportError`).

### Contexts: `ContextScope`

```cpp
enum class ConsoleLevel : std::uint8_t { Log, Info, Debug, Warn, Error };
using ConsoleSink = std::function<void(ConsoleLevel level, std::string_view text)>;
struct ContextOptions {
    bool mConsole = true;                  // console.*, formatted like Node's
    ConsoleSink mConsoleSink{};            // default: log / info / debug to stdout, warn / error to stderr
    bool mTimers = true;                   // setTimeout / setInterval / setImmediate / clear*
    bool mWebGlobals = true;               // see Context globals
    std::filesystem::path mModuleRoot{};   // base for non-file module sources; default: working directory
};
struct EvalOptions { std::chrono::milliseconds mTimeout{0}; };    // 0: none
class ContextScope {
    explicit ContextScope(v8::Isolate* isolate, const ContextOptions& options = {});
    v8::Isolate* isolate() const noexcept;  v8::Local<v8::Context> context() const;
    template<BoundModule... Mods> void install() const;
    template<BoundModule... Mods> void registerModules() const;
    Result<Value> eval(std::string_view source, std::string_view scriptName = "<eval>", const EvalOptions& = {}) const;
    Result<Value> evalModule(std::string_view source, std::string_view moduleName = "<module>",
                             const EvalOptions& = {}) const;              // the module namespace
    Result<Value> runModuleFile(const std::filesystem::path& path, const EvalOptions& = {}) const;
    Result<std::string> evalToString(std::string_view source, std::string_view scriptName = "<eval>",
                                     const EvalOptions& = {}) const;
};
```

- A stack object on the isolate thread: it enters the isolate, opens a HandleScope, creates and enters a new context; on destruction the context's timers and module records go too.
- Errors are `ConvertError`s carrying the thrown value; a timeout reads `the script timed out after N ms`.
- `evalModule` drains microtasks once, so a module without pending top-level awaits has settled; a rejected evaluation is an error. `moduleName` counts as a file path (imports resolve next to it) when it contains `/` or `\` or ends in `.js` / `.mjs`, and does not start with `<`. `runModuleFile` runs a file under its absolute path.

### Context globals and module resolution

Implemented in `src/Engine/WebGlobals.cpp`, `src/Engine/Engine.cpp` and `src/Engine/ModuleLoader.cpp`:

- `console`: `log`, `info`, `debug`, `warn`, `error`, `trace`, `assert`, `dir`, `table`, `count`, `countReset`, `time`, `timeLog`, `timeEnd`, `group`, `groupCollapsed`, `groupEnd`; specifiers `%s %d %i %f %j %o %O %c %%`. Each call also reaches V8's built-in console, so an attached inspector sees it.
- Timers: `setTimeout`, `setInterval`, `setImmediate` (extra arguments go to the callback; ids are numbers), `clearTimeout`, `clearInterval`, `clearImmediate`; run by `pump()`.
- Web globals: `queueMicrotask`, `reportError`, `structuredClone(value, {transfer})` (V8's serializer; transferred ArrayBuffers move and the originals detach), `performance` (`now()`, `timeOrigin`), `TextEncoder` (`encode`, `encodeInto`), `TextDecoder` (UTF-8 only; `fatal`, `ignoreBOM`, `{stream: true}`).
- `Event` / `EventTarget` (`once`, `signal`, `capture` options; no propagation), `AbortController` / `AbortSignal` (`abort`, `timeout(ms)` which does not keep the loop alive, `any`, `throwIfAborted`, `onabort`, `reason`). Errors thrown by event listeners and `queueMicrotask` callbacks are reported, not propagated.

Module specifiers in `evalModule`, `runModuleFile` and `import()` resolve in this order:

1. A name registered with `registerModules` is that Alka module.
2. Relative (`./`, `../`) and absolute paths and `file://` URLs are files, relative to the importing module's file, else to `mModuleRoot`; `import()` in a script resolves next to the script when its name is the absolute path of an existing file.
3. Tried in order: the path, then with `.mjs`, `.js`, then `index.mjs`, `index.js` inside it. `.json` files export the parsed data as `default`.
4. Other bare specifiers fail with `Cannot find module '<specifier>'`.

A file loads once per context (by canonical path); `import.meta.url` is its `file://` URL (a source module's name otherwise).

### `Inspector`

A Chrome DevTools Protocol endpoint for a self-hosted isolate; no transport included. Header: `<Alka/Engine/Inspector.hpp>`

```cpp
Alka::Inspector inspector(isolate);
inspector.contextCreated(context, "main");          // contextDestroyed(context) when it goes
auto session = inspector.connect([&](std::string_view json) { socket.send(json); });
session->dispatch(incomingJson);                     // isolate thread (post through a Dispatcher)
inspector.setPausePump(fn);                          // what runs, repeatedly, while paused
inspector.onFrontendReady(fn);                       // the front end sent Runtime.runIfWaitingForDebugger
session->schedulePauseOnNextStatement();             // e.g. stop at a script's first line
```

- Outgoing messages (UTF-8 JSON) reach `send` on the isolate thread.
- While paused at a breakpoint V8 blocks inside the inspector; the pause pump (default: `Engine::pump` plus a Dispatcher drain) keeps delivering messages posted to the isolate's Dispatcher, so a transport thread can send `Debugger.resume`. `tools/InspectorBridge` is a WebSocket transport for Chrome DevTools and VS Code.

## 11. Node.js / Electron

Alka inside a Node addon (Electron included), on Node's own V8. Header-only: `<Alka/Host/Node.hpp>` (needs `node.h` and `uv.h`; node-gyp or cmake-js)

```cpp
NODE_MODULE_INIT(/* exports, module, context */) {
    Alka::Node::attach(context);                    // first, once per environment
    Alka::installInto<Geometry>(context, exports);  // or Alka::install<Geometry>(context)
}
```

- `attach(context)` wires the isolate's `Dispatcher` to the environment's libuv loop. Work posted from other threads (settler results, offloaded `Task` steps, `ThreadSafeFunction` calls) runs on the loop thread inside a `node::CallbackScope`, followed by Node's nextTick queue and microtasks. Idempotent per environment; call it on the loop thread.
- In-flight work keeps the process alive like a ref'd libuv handle; with nothing pending, nothing is held.
- When the environment shuts down (the main thread at exit, a `worker_threads` Worker when it ends) Alka calls `disposeIsolate`: native objects JS still held are destroyed.
- Build Alka with `ALKA_BUILD_ENGINE=OFF` against Node's V8 headers, with node.h's definitions for every translation unit, Alka's own included: `BUILDING_NODE_EXTENSION`, `USING_V8_SHARED`, `USING_UV_SHARED`.
- With clang 17+ on Linux / macOS also define `COMPONENT_BUILD`: V8's headers otherwise declare cppgc's slow paths `preserve_most`, which Linux Node releases (built with GCC) do not implement; the ordinary convention is safe either way.
- Windows: link the release's `node.lib` (as `tests/Node/CMakeLists.txt` does). `USING_V8_SHARED` also meets the Windows condition for `GcObject` classes (which still need V8 13+); with MSVC, Alka supplies the `v8::SourceLocation::Current()` import that clang-cl-built Node 23+ / Electron import libraries lack.
- Some header sets leave out `v8-fast-api-calls.h`; `ALKA_FAST_API` then defaults to 0.
- **Electron** (verified with 45, V8 15.6, Linux x64): V8's C++ API passes `std` types, and Electron's are those of its own libc++ (`std::__Cr`). Compile with Chromium's clang of that release against the release's libc++ headers, link its libc++ objects (ThinLTO bitcode) with lld, define the V8 ABI macros of its `config.gypi` (pointer compression, the sandbox) and `ALKA_HAS_CPPGC=0` (Electron exports no cppgc allocation internals). `ci/electron-addon.sh` is the complete recipe; see the Cookbook.

## 12. Configuration macros

Defined in `<Alka/Config.hpp>`; override with `target_compile_definitions` before any Alka header. Alka's compiled sources read several of them (`ALKA_HAS_CPPGC`, `ALKA_CONTEXT_EMBEDDER_*`, `ALKA_EMBEDDER_TAG_CONTEXT`), so give the Alka target and its consumers the same values.

| Macro | Default | Meaning |
|---|---|---|
| `ALKA_VERSION_MAJOR` / `MINOR` / `PATCH` | 0 / 2 / 0 | version numbers |
| `ALKA_VERSION_STRING` | `"0.2.0"` | also `versionString()` |
| `ALKA_ENUM_RANGE_MIN` / `MAX` | -128 / 128 | default enum reflection range; must include 0 and span at most 4096; per enum: `EnumRange<E>` |
| `ALKA_INT64_MODE` | 0 | default `Int64Mode` for results: 0 `BigInt`, 1 `Number`, 2 `Auto` |
| `ALKA_FAST_API` | 1 if `<v8-fast-api-calls.h>` exists, else 0 | generate V8 Fast API paths |
| `ALKA_FAST_API_STATS` | 0 | count fast calls in `Alka::Detail::gFastCalls` (tests) |
| `ALKA_STRICT_BORROW` | 1 | refuse unvouched `T*` / `T&` lending at compile time |
| `ALKA_NAMING` / `ALKA_NAMING_HEADER` | `Alka::verbatim` / unset | the naming of bindings that declare none ([Naming](#naming-and-reflection)) |
| `ALKA_HAS_CPPGC` | 1 with cppgc headers, V8 >= 13 and (not Windows or `USING_V8_SHARED`); else 0 | `GcObject` support; with a static V8 on Windows define 1 |
| `ALKA_CPPHEAP_TAG` | V8 13 / 14: `kDefaultTag` (`0x7000`); 15+: `kLastEmbedderWrappableTag` | `CppHeapPointerTag` for wrapping C++-heap objects (V8 15 checks wrappables against its embedder range and has no default under the sandbox) |
| `ALKA_EXTERNAL_TAG` | `V8_EXTERNAL_POINTER_TAG_COUNT` - 4 | first of the three `ExternalPointerTypeTag`s (V8 15+) of Alka's `v8::External`s: native functions' data, generator iterators', promise awaiters' |
| `ALKA_CONTEXT_EMBEDDER_SLOT` | 64 | first context embedder-data slot Alka may use (past V8, gin / Blink and Node) |
| `ALKA_CONTEXT_EMBEDDER_PROBES` | 8 | slots probed from there; only slots holding Alka's own data are trusted, so several Alka copies coexist |
| `ALKA_EMBEDDER_TAG_TYPE_NODE` / `HOLDER` / `CONTEXT` | 12 / 13 / 14 | embedder-data type tags (checked by V8 14 with its sandbox); 0 to `V8_EMBEDDER_DATA_TAG_COUNT` - 1 |

Detected, not meant to be set: `ALKA_HAS_EXTERNAL_MEMORY_ACCOUNTER` (`<v8-external-memory-accounter.h>` present; `Detail/State.hpp`) and `ALKA_HAS_GENERATOR` (`std::generator` available; `Detail/Invoke.hpp`). Macros used in code: `ALKA_OVERRIDE`, `ALKA_OVERRIDE_PURE` ([Virtual overrides](#virtual-overrides-trampoline)).
