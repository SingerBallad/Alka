#include <Alka/Alka.hpp>
#include <Alka/Engine/Engine.hpp>

#include <v8.h>
#if ALKA_FAST_API
#include <v8-fast-api-calls.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

// What Alka costs per operation next to hand-written V8 bindings doing the
// same work (with the type checks a careful hand-written binding makes):
// time, and C++ heap allocations (V8 allocates its own way, uncounted).
//   AlkaBench [filter]
// Build against a Release V8 for meaningful numbers: a Debug V8 is many times
// slower everywhere.

namespace {
thread_local std::uint64_t tAllocations = 0;
} // namespace

// (sanitizers bring their own operator new: no counts there)
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ALKA_BENCH_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define ALKA_BENCH_SANITIZED 1
#endif
#endif

#ifndef ALKA_BENCH_SANITIZED
void* operator new(const std::size_t size) {
    ++tAllocations;
    if (void* p = std::malloc(size != 0 ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](const std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
#endif

namespace Bench {

int add(const int a, const int b) {
    return a + b;
}
int addRegular(const int a, const int b) {
    return a + b;
} // (bound with noFast)
int byteLength(std::string_view text) {
    return static_cast<int>(text.size());
}
std::string greet(const std::string& name) {
    return "hi " + name;
}
double sum(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0);
}
std::vector<int> range(const int n) {
    std::vector<int> out(static_cast<std::size_t>(std::max(n, 0)));
    std::iota(out.begin(), out.end(), 0);
    return out;
}

struct Point {
    Point(const double x, const double y) : mX(x), mY(y) {}
    [[nodiscard]] double length() const { return std::sqrt(mX * mX + mY * mY); }
    double mX = 0;
    double mY = 0;
};
Point makePoint(const double x, const double y) {
    return {x, y};
}

double callMany(const Alka::Function<double(double)>& fn, const int n) {
    double total = 0;
    for (int i = 0; i < n; ++i) {
        total += fn.call(static_cast<double>(i));
    }
    return total;
}

struct Mod;

} // namespace Bench

template <> struct Alka::Bind<Bench::Point> : Alka::Class<Bench::Point> {
    static constexpr auto members =
        Alka::members(Alka::ctor<double, double>(), Alka::prop<"x", &Bench::Point::mX>(),
                      Alka::prop<"y", &Bench::Point::mY>(), Alka::method<&Bench::Point::length>());
};

template <> struct Alka::Bind<Bench::Mod> : Alka::Module<"alka"> {
    static constexpr auto members =
        Alka::members(Alka::type<Bench::Point>, Alka::fn<&Bench::add>(),
                      Alka::fn<&Bench::addRegular>(Alka::noFast), Alka::fn<&Bench::byteLength>(),
                      Alka::fn<&Bench::greet>(), Alka::fn<&Bench::sum>(), Alka::fn<&Bench::range>(),
                      Alka::fn<&Bench::makePoint>(), Alka::fn<&Bench::callMany>());
};

// ---- the same, by hand ------------------------------------------------------------

namespace Raw {

void throwType(v8::Isolate* isolate, const char* message) {
    isolate->ThrowException(
        v8::Exception::TypeError(v8::String::NewFromUtf8(isolate, message).ToLocalChecked()));
}

void add(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length() < 2 || !info[0]->IsInt32() || !info[1]->IsInt32()) {
        return throwType(info.GetIsolate(), "add(int, int)");
    }
    info.GetReturnValue().Set(info[0].As<v8::Int32>()->Value() + info[1].As<v8::Int32>()->Value());
}

#if ALKA_FAST_API
// V8's own fastest shape: int32 in and out, no validation at all.
std::int32_t fastAdd(v8::Local<v8::Value>, const std::int32_t a, const std::int32_t b) {
    return a + b;
}
#endif

void byteLength(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length() < 1 || !info[0]->IsString()) {
        return throwType(info.GetIsolate(), "byteLength(string)");
    }
    const v8::String::Utf8Value text(info.GetIsolate(), info[0]);
    info.GetReturnValue().Set(text.length());
}

void greet(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    if (info.Length() < 1 || !info[0]->IsString()) {
        return throwType(isolate, "greet(string)");
    }
    const v8::String::Utf8Value name(isolate, info[0]);
    const std::string text = "hi " + std::string(*name, static_cast<std::size_t>(name.length()));
    info.GetReturnValue().Set(v8::String::NewFromUtf8(isolate, text.data(),
                                                      v8::NewStringType::kNormal,
                                                      static_cast<int>(text.size()))
                                  .ToLocalChecked());
}

void sum(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    if (info.Length() < 1 || !info[0]->IsArray()) {
        return throwType(isolate, "sum(number[])");
    }
    const auto context = isolate->GetCurrentContext();
    const auto array = info[0].As<v8::Array>();
    std::vector<double> values(array->Length());
    for (std::uint32_t i = 0; i < array->Length(); ++i) {
        v8::Local<v8::Value> item;
        if (!array->Get(context, i).ToLocal(&item) || !item->IsNumber()) {
            return throwType(isolate, "sum(number[])");
        }
        values[i] = item.As<v8::Number>()->Value();
    }
    info.GetReturnValue().Set(std::accumulate(values.begin(), values.end(), 0.0));
}

void range(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    if (info.Length() < 1 || !info[0]->IsInt32()) {
        return throwType(isolate, "range(int)");
    }
    const auto values = Bench::range(info[0].As<v8::Int32>()->Value());
    std::vector<v8::Local<v8::Value>> elements;
    elements.reserve(values.size());
    for (const int v : values) {
        elements.push_back(v8::Integer::New(isolate, v));
    }
    info.GetReturnValue().Set(v8::Array::New(isolate, elements.data(), elements.size()));
}

// Point: an internal field, a weak handle that deletes it -- the minimum a
// hand-written binding needs not to leak or dangle.
struct Wrapped {
    Bench::Point mPoint;
    v8::Global<v8::Object> mHandle;
};

void weakCallback(const v8::WeakCallbackInfo<Wrapped>& info) {
    auto* wrapped = info.GetParameter();
    wrapped->mHandle.Reset();
    delete wrapped;
}

void attach(v8::Isolate* isolate, const v8::Local<v8::Object> object, const double x,
            const double y) {
    auto* wrapped = new Wrapped{Bench::Point(x, y), {}};
    // (V8 14 wants a type tag here: Alka's version-proof helper, nothing more)
    Alka::Detail::setInternalPointer(object, 0, wrapped, Alka::Detail::fieldTag(0));
    wrapped->mHandle.Reset(isolate, object);
    wrapped->mHandle.SetWeak(wrapped, &weakCallback, v8::WeakCallbackType::kParameter);
}

v8::Global<v8::FunctionTemplate> gPointClass;

void construct(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    if (!info.IsConstructCall() || info.Length() < 2 || !info[0]->IsNumber() ||
        !info[1]->IsNumber()) {
        return throwType(isolate, "new Point(number, number)");
    }
    attach(isolate, info.This(), info[0].As<v8::Number>()->Value(),
           info[1].As<v8::Number>()->Value());
}

auto pointOf(const v8::Local<v8::Value> self) -> Bench::Point* {
    if (!self->IsObject() || self.As<v8::Object>()->InternalFieldCount() < 1) {
        return nullptr;
    }
    return &static_cast<Wrapped*>(
                Alka::Detail::internalPointer(self.As<v8::Object>(), 0, Alka::Detail::fieldTag(0)))
                ->mPoint;
}

void getX(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (const auto* point = pointOf(info.This())) {
        info.GetReturnValue().Set(point->mX);
    }
}

void length(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (const auto* point = pointOf(info.This())) {
        info.GetReturnValue().Set(point->length());
    }
}

void makePoint(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    if (info.Length() < 2 || !info[0]->IsNumber() || !info[1]->IsNumber()) {
        return throwType(isolate, "makePoint(number, number)");
    }
    const auto context = isolate->GetCurrentContext();
    const auto object =
        gPointClass.Get(isolate)->InstanceTemplate()->NewInstance(context).ToLocalChecked();
    attach(isolate, object, info[0].As<v8::Number>()->Value(), info[1].As<v8::Number>()->Value());
    info.GetReturnValue().Set(object);
}

void callMany(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    if (info.Length() < 2 || !info[0]->IsFunction() || !info[1]->IsInt32()) {
        return throwType(isolate, "callMany(function, int)");
    }
    const auto context = isolate->GetCurrentContext();
    const auto fn = info[0].As<v8::Function>();
    const int n = info[1].As<v8::Int32>()->Value();
    double total = 0;
    for (int i = 0; i < n; ++i) {
        v8::Local<v8::Value> argv[] = {v8::Number::New(isolate, i)};
        v8::Local<v8::Value> result;
        if (!fn->Call(context, v8::Undefined(isolate), 1, argv).ToLocal(&result)) {
            return; // (the exception propagates)
        }
        if (!result->IsNumber()) {
            return throwType(isolate, "callMany: the function must return numbers");
        }
        total += result.As<v8::Number>()->Value();
    }
    info.GetReturnValue().Set(total);
}

void install(v8::Isolate* isolate, const v8::Local<v8::Context> context) {
    const auto raw = v8::Object::New(isolate);
    const auto define = [&](const char* name, const v8::Local<v8::FunctionTemplate> ft) {
        raw->Set(context, v8::String::NewFromUtf8(isolate, name).ToLocalChecked(),
                 ft->GetFunction(context).ToLocalChecked())
            .Check();
    };
    const auto fn = [&](const char* name, const v8::FunctionCallback callback,
                        const v8::CFunction* fast = nullptr) {
        define(name, v8::FunctionTemplate::New(isolate, callback, {}, {}, 0,
                                               v8::ConstructorBehavior::kThrow,
                                               v8::SideEffectType::kHasSideEffect, fast));
    };
    fn("add", &add);
#if ALKA_FAST_API
    static const auto fastAddFunction = v8::CFunction::Make(&fastAdd);
    fn("addFast", &add, &fastAddFunction);
#else
    fn("addFast", &add);
#endif
    fn("byteLength", &byteLength);
    fn("greet", &greet);
    fn("sum", &sum);
    fn("range", &range);
    fn("makePoint", &makePoint);
    fn("callMany", &callMany);

    const auto point = v8::FunctionTemplate::New(isolate, &construct);
    point->SetClassName(v8::String::NewFromUtf8Literal(isolate, "Point"));
    point->InstanceTemplate()->SetInternalFieldCount(1);
    const auto signature = v8::Signature::New(isolate, point);
    point->PrototypeTemplate()->SetAccessorProperty(
        v8::String::NewFromUtf8Literal(isolate, "x"),
        v8::FunctionTemplate::New(isolate, &getX, {}, signature));
    point->PrototypeTemplate()->Set(isolate, "length",
                                    v8::FunctionTemplate::New(isolate, &length, {}, signature));
    gPointClass.Reset(isolate, point);
    define("Point", point);
    context->Global()->Set(context, v8::String::NewFromUtf8Literal(isolate, "raw"), raw).Check();
}

} // namespace Raw

// ---- the harness --------------------------------------------------------------------

namespace {

struct Case {
    const char* mName;
    const char* mSetup; // runs once before the loop (may be empty)
    const char* mBody;  // an expression over `ns` (alka / raw) and `i`, added to the total
    double mOps;        // operations per run
};

struct Measured {
    double mNs = -1;         // per operation, the best run
    double mAllocations = 0; // C++ heap allocations per operation
};

// Best of a few timed runs of `ops` operations.
Measured measure(const Alka::ContextScope& scope, const Case& c, const char* ns) {
    const std::string source = std::string("(() => { const ns = ") + ns + "; " + c.mSetup +
                               "; let t = 0; for (let i = 0; i < " +
                               std::to_string(static_cast<long long>(c.mOps)) +
                               "; ++i) { t += " + c.mBody + "; } return t; })()";
    Measured best;
    for (int run = 0; run < 7; ++run) {
        const auto allocations = tAllocations;
        const auto start = std::chrono::steady_clock::now();
        const auto result = scope.eval(source);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (!result) {
            std::fprintf(stderr, "%s (%s): %s\n", c.mName, ns, result.error().message().c_str());
            return {};
        }
        const double perOp = std::chrono::duration<double, std::nano>(elapsed).count() / c.mOps;
        if (run == 0 || perOp < best.mNs) {
            best.mNs = perOp;
        }
        if (run == 0) {
            best.mAllocations = static_cast<double>(tAllocations - allocations) / c.mOps;
        }
    }
    return best;
}

} // namespace

int main(const int argc, char** argv) {
    const std::string_view filter = argc > 1 ? argv[1] : "";
    auto& engine = Alka::Engine::instance();
    (void)engine;
    auto isolate = Alka::Engine::createIsolate();
    const Alka::ContextScope scope(isolate.get());
    scope.install<Bench::Mod>();
    Raw::install(isolate.get(), scope.context());

    const Case cases[] = {
        // Alka's fast path against a hand-written callback, both regular
        // paths, and against V8's bare Fast API (no argument checks at all)
        {"add(int, int)", "", "ns.add(i, 1)", 2e6},
        {"add, both regular paths", "", "(ns === alka ? ns.addRegular(i, 1) : ns.add(i, 1))", 2e6},
        {"add vs bare raw Fast API", "", "(ns === alka ? ns.add(i, 1) : ns.addFast(i, 1))", 2e6},
        {"string in (16 chars)", "const s = 'abcdefghijklmnop'", "ns.byteLength(s)", 1e6},
        {"string in (two-byte)", "const s = '\\u4f60\\u597d\\u4e16\\u754c'", "ns.byteLength(s)",
         1e6},
        {"string in + out", "", "ns.greet('world').length", 5e5},
        {"array in (1000 numbers)", "const a = Array.from({length: 1000}, (_, k) => k)",
         "ns.sum(a)", 2e4},
        {"array out (1000 ints)", "", "ns.range(1000).length", 2e4},
        {"new object", "", "new ns.Point(i, 1).x", 5e5},
        {"object from C++ (owned)", "", "ns.makePoint(i, 1).x", 5e5},
        {"property get", "const p = new ns.Point(3, 4)", "p.x", 2e6},
        {"method call", "const p = new ns.Point(3, 4)", "p.length()", 2e6},
        {"C++ calls JS (per call)", "const f = (x) => x + 1", "ns.callMany(f, 1000)", 2e3},
    };

    std::printf("Alka %s on V8 %s\n", Alka::versionString(), v8::V8::GetVersion());
    std::printf("%-28s %9s %9s %7s %12s %11s\n", "operation", "Alka ns", "raw ns", "ratio",
                "Alka allocs", "raw allocs");
    for (const auto& c : cases) {
        if (!filter.empty() && std::string_view(c.mName).find(filter) == std::string_view::npos) {
            continue;
        }
        auto alka = measure(scope, c, "alka");
        auto raw = measure(scope, c, "raw");
        if (std::string_view(c.mName).starts_with("C++ calls JS")) {
            for (auto* m : {&alka, &raw}) { // (each operation is 1000 calls)
                m->mNs /= 1000;
                m->mAllocations /= 1000;
            }
        }
        std::printf("%-28s %9.1f %9.1f %7.2f %12.2f %11.2f\n", c.mName, alka.mNs, raw.mNs,
                    raw.mNs > 0 ? alka.mNs / raw.mNs : 0.0, alka.mAllocations, raw.mAllocations);
        std::fflush(stdout);
    }
    Raw::gPointClass.Reset();
    return 0;
}
