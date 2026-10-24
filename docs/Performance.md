# Performance

What a call through Alka costs, where that time goes, and how to make hot
paths cheaper. The numbers come from `benchmarks/AlkaBench`, which runs every
case twice: once through Alka, once through a hand-written V8 binding of the
same C++ function.

## How it is measured

- Each case is a JS loop of 2,000 to 2,000,000 calls into a bound function. The
  same loop then runs against the hand-written binding (`raw`). Timing is the
  best of 7 runs, per operation.
- The hand-written bindings are what a careful author writes: they check
  argument types and throw `TypeError`s, and give objects an internal field and
  a weak handle that deletes them. They do not do what Alka adds on top (see
  below), so the gap is the price of those guarantees.
- C++ heap allocations are counted by replacing `operator new`. V8's own heap is
  not counted.
- Build: `-DALKA_BUILD_BENCHMARKS=ON`, which is the default for a top-level
  build and needs the Engine layer. Run `AlkaBench [filter]`. Only a Release V8
  gives meaningful numbers, because a Debug V8 is many times slower
  everywhere. Comparisons between versions of Alka must run builds alternately
  on the same machine (A B B A …); runs made one after another drift by 10 % or
  more.

## Results

Intel Core i9-13900H laptop. The lists below give ns per operation for Alka
and the hand-written binding, then C++ heap allocations per operation.

### Windows: MSVC 19.5x, `/O2`, V8 13.0.245.25 (NuGet, Release)

Calls:

- `add(int, int)`, Alka via the Fast API: 13.6 ns, hand-written 11.8 ns. 0 allocations either way.
- `add`, both on regular callbacks: 37.0 ns, hand-written 13.1 ns. 0 allocations.
- `add`, against V8's bare Fast API (no checks): 14.6 ns, hand-written 4.8 ns. 0 allocations.
- Property get: 35.0 ns, hand-written 13.8 ns. 0 allocations.
- Method call: 31.5 ns, hand-written 15.0 ns. 0 allocations.

Strings and arrays:

- `string_view` in (16 chars): 57.1 ns, hand-written 57.2 ns. 0 allocations.
- `string_view` in (two-byte): 55.7 ns, hand-written 63.4 ns. 0 allocations.
- String in + out: 71.9 ns, hand-written 96.2 ns. 0 allocations.
- Array in (1000 numbers): 6,860 ns, hand-written 26,860 ns. 1 allocation each.
- Array out (1000 ints): 4,108 ns, hand-written 4,042 ns. 2 allocations each.

Objects and calls back into JS:

- New object (constructed by JS): 466 ns, hand-written 172 ns. Alka 3 allocations, hand-written 1.
- Object returned from C++ (owned): 506 ns, hand-written 153 ns. Alka 3 allocations, hand-written 1.
- C++ calls a JS function: 76.5 ns, hand-written 41.7 ns. 0 allocations.

### Linux: GCC 15, `-O3`, V8 12.4 (Ubuntu's libnode)

V8 12.4 cannot throw from a fast call, so Alka never takes the Fast API there
(see [the Fast API](#the-fast-api)): `add` runs on the regular path.

Calls:

- `add(int, int)`, regular path: 21.2 ns, hand-written 11.6 ns. 0 allocations either way.
- Property get: 20.6 ns, hand-written 11.6 ns. 0 allocations.
- Method call: 32.9 ns, hand-written 11.8 ns. 0 allocations.

Strings and arrays:

- `string_view` in (16 chars): 50.2 ns, hand-written 34.7 ns. Alka 0 allocations, hand-written 1.
- String in + out: 66.9 ns, hand-written 63.8 ns. Alka 0 allocations, hand-written 1.
- Array in (1000 numbers): 5,712 ns, hand-written 25,865 ns. 1 allocation each.
- Array out (1000 ints): 2,102 ns, hand-written 2,126 ns. 2.3 allocations each.

Objects and calls back into JS:

- New object (constructed by JS): 270 ns, hand-written 104 ns. Alka 3 allocations, hand-written 1.
- Object returned from C++ (owned): 299 ns, hand-written 114 ns. Alka 3 allocations, hand-written 1.
- C++ calls a JS function: 77.6 ns, hand-written 41.0 ns. 0 allocations.

The hand-written string cases show 0 allocations on Windows and 1 on Linux,
but they do the same work on both. `String::Utf8Value` allocates its buffer
inside V8. On Linux the executable's counting `operator new` also replaces
libnode's. A V8 DLL on Windows keeps its own, so the counter misses it there.

## Where the time goes

**Every call.** Alka checks that the call runs on the isolate's thread. It then
finds the context's bindings through a thread-local cache. Each argument is
converted and validated: `1.5` is not an `int`, `2**40` does not fit in one,
and a string is not a number. C++ exceptions are caught and turned into JS
errors. On a regular `add` this work is the difference between 37 ns and 13 ns
(Windows) or 21 ns and 12 ns (Linux).

**Receivers.** A method or accessor reads the object's holder and checks it:

- The class, so a `Point` method called on another object throws instead of
  reinterpreting memory.
- That the object is alive: not destroyed (`Trackable`), not expired
  (`weak_ptr`), not disposed.
- For a part of another object, that the part can still be found.

A hand-written binding with a `v8::Signature` makes only the first of these
checks.

**Objects entering JS** cost the most. The identity cache maps each C++ object
to its one JS object, so `===`, expandos and JS subclasses work. It accounts for
about 90 ns of the gap. Alka also creates three records where a hand-written
binding creates one:

- The object itself. JS-created objects live in a `shared_ptr`, so C++ can later
  share them safely.
- The holder, which records who owns the object.
- An entry in the isolate's registry, so `disposeIsolate` can release
  everything and weak callbacks find their object.

**C++ calling JS** converts each argument and the result. It enters the
isolate if it is not already current, and runs the call under a `TryCatch`
so a JS exception becomes a C++ one.

**Where Alka is faster.** Arrays of numbers convert in one `Array::Iterate`
pass instead of an element-by-element `Get`. Short strings convert in one pass
into a 120-byte stack buffer (`InlineUtf8`). `Utf8Value` measures the string,
allocates, then copies.

## The Fast API

TurboFan can call an eligible bound callable directly from optimized JS,
skipping the callback machinery. The `add(int, int)` row on Windows shows the
result: 13.6 ns, close to a hand-written regular callback and within 10 ns of
V8's bare, unchecked fast call. A callable is eligible when:

- It has one overload, with no defaults, no `Rest<T>`, no injected parameters,
  no policies and no `keepAlive`.
- Its parameters are `bool`, integers up to 32 bits, `float`, `double`,
  `std::string` or `std::string_view`.
- It returns `void`, `bool` or one of those numbers.

Integers arrive as doubles and go through the regular path's validation, so
both tiers accept and reject the same values with the same messages. Strings
take the fast path when V8 holds them as flat one-byte strings. V8 13.0 itself
fast-calls a string function only when it takes one string and returns
nothing. Other string shapes stay on the regular path there.

Older V8 versions, up to about 12.5, give a fast call no isolate, so it cannot
throw. It can only fall back to the regular callback, which would run a C++
call twice after an exception. Alka detects this from V8's headers and
registers no fast paths there. That is why `add` runs on the regular path in
the Linux numbers above. `Alka::noFast` opts one declaration out, and
`ALKA_FAST_API=0` turns the Fast API off everywhere.

## Making hot paths cheaper

- **Batch across the boundary.** One call taking a `std::span<const double>` or
  an array beats a thousand calls taking one number. The fixed cost per call
  (10–40 ns) dominates small functions.
- **Shape hot functions for the Fast API.** Use a single overload with numbers,
  bools and strings, and no defaults.
- **Take `std::string_view`** rather than `const std::string&`, which allocates
  for strings longer than the small-string buffer.
- **Use typed arrays for bulk data.** `std::span<T>` parameters view a typed
  array in place. `Alka::TypedView<T>` keeps the memory, zero-copy, and
  `BufferView` can be handed to another thread. A JS array is always copied.
- **Prefer records for small value types** such as points, rectangles and
  options. `Alka::Record<T>` converts to and from a plain JS object with no
  identity, holder or registry. Classes are right when identity, methods or
  sharing matter.
- **Mind the objects crossing over.** Each new object entering JS costs
  300–500 ns. Returning a C++ object JS has already seen costs only the
  identity lookup. Returning by value makes a new object every time.
- **Hoist `Alka::Function` calls out of tight loops** when the JS side can take
  a batch. Each call into JS costs about 75 ns.
