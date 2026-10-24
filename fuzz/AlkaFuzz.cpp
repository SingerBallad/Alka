#include <Alka/Alka.hpp>
#include <Alka/Engine/Engine.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace Fz {

struct Point {
    Point() = default;
    explicit Point(const int x) : mX(x) {}
    int mX = 0;
};

struct Polygon {
    std::vector<Point> mPoints;
    std::vector<int> mNumbers;
    Point mOrigin;
    void add(const int x) {
        if (mPoints.size() < 64) {
            mPoints.emplace_back(x);
        }
    }
    void clear() { mPoints.clear(); }
    void shrink() {
        if (!mPoints.empty()) {
            mPoints.pop_back();
        }
    }
    [[nodiscard]] std::size_t size() const { return mPoints.size(); }
    Point& operator[](const std::size_t i) { return mPoints[i]; }
    Point& at(const int i) { return mPoints.at(static_cast<std::size_t>(i)); }
};

class Animal {
public:
    virtual ~Animal() = default;
    [[nodiscard]] virtual std::string speak() const { return "..."; }
    [[nodiscard]] virtual int legs() const { return 4; }
};

struct JsAnimal : Animal, Alka::Overridable<Animal> {
    [[nodiscard]] std::string speak() const override {
        return ALKA_OVERRIDE(std::string, Animal, speak);
    }
    [[nodiscard]] int legs() const override { return ALKA_OVERRIDE(int, Animal, legs); }
};

class Dog : public Animal {
public:
    explicit Dog(std::string name) : mName(std::move(name)) {}
    [[nodiscard]] std::string speak() const override { return mName + ": woof"; }
    std::string mName;
};

struct Kennel {
    std::vector<std::shared_ptr<Animal>> mAnimals;
    Animal* animal(const int i) {
        return i >= 0 && i < static_cast<int>(mAnimals.size())
                   ? mAnimals[static_cast<std::size_t>(i)].get()
                   : nullptr;
    }
    void putDog(const int i, std::string name) { put(i, std::make_shared<Dog>(std::move(name))); }
    void adopt(std::shared_ptr<Animal> animal) {
        if (animal && mAnimals.size() < 16) {
            mAnimals.push_back(std::move(animal));
        }
    }
    void clear() { mAnimals.clear(); }
    [[nodiscard]] std::string chorus() const {
        std::string out;
        for (std::size_t i = 0; i < mAnimals.size() && out.size() < 4096; ++i) {
            if (const auto& animal = mAnimals[i]) {
                out += animal->speak();
                out += std::to_string(animal->legs());
            }
        }
        return out;
    }

private:
    void put(const int i, std::shared_ptr<Animal> animal) {
        const auto at = static_cast<std::size_t>(i & 15);
        if (mAnimals.size() <= at) {
            mAnimals.resize(at + 1);
        }
        mAnimals[at] = std::move(animal);
    }
};

struct File {
    bool mOpen = true;
    void close() { mOpen = false; }
    [[nodiscard]] std::string read() const {
        if (!mOpen) {
            throw std::runtime_error("closed");
        }
        return "data";
    }
};

struct Enemy : Alka::Trackable {
    int mHp = 10;
};
inline std::vector<std::unique_ptr<Enemy>> gEnemies;
inline Enemy* spawn() {
    if (gEnemies.size() < 32) {
        gEnemies.push_back(std::make_unique<Enemy>());
    }
    return gEnemies.back().get();
}
inline void killAll() {
    gEnemies.clear();
}

inline std::shared_ptr<Polygon> gShared;
inline std::shared_ptr<Polygon> share(std::shared_ptr<Polygon> polygon) {
    gShared = std::move(polygon);
    return gShared;
}
inline void dropShared() {
    gShared.reset();
}
inline std::unique_ptr<Polygon> makePolygon(const int n) {
    auto polygon = std::make_unique<Polygon>();
    for (int i = 0; i < (n & 15); ++i) {
        polygon->add(i);
    }
    return polygon;
}
inline std::string echo(const std::string_view text) {
    return std::string(text);
}
inline double total(const std::span<const double> values) {
    double sum = 0;
    for (const double v : values) {
        sum += v;
    }
    return sum;
}
inline std::vector<int> range(const int n) {
    std::vector<int> out(static_cast<std::size_t>(n & 63));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<int>(i);
    }
    return out;
}
inline int call(const Alka::Function<int(int)>& fn, const int x) {
    return fn.call(x);
}

struct Mod;

} // namespace Fz

template <> struct Alka::Bind<Fz::Point> : Class<Fz::Point> {
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(), Alka::prop<"x", &Fz::Point::mX>());
};

template <> struct Alka::Bind<Fz::Polygon> : Class<Fz::Polygon> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::method<&Fz::Polygon::add>(), Alka::method<&Fz::Polygon::clear>(),
        Alka::method<&Fz::Polygon::shrink>(), Alka::method<&Fz::Polygon::at>(policy::internalRef),
        Alka::prop<"origin", &Fz::Polygon::mOrigin>(),
        Alka::prop<"numbers", &Fz::Polygon::mNumbers>(), Alka::indexer(policy::internalRef),
        Alka::iterable<Fz::Polygon>(policy::internalRef));
};

template <> struct Alka::Bind<Fz::Animal> : Class<Fz::Animal> {
    using Trampoline = Fz::JsAnimal;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"speak", &Fz::Animal::speak>(),
                      Alka::method<"legs", &Fz::Animal::legs>());
};

template <> struct Alka::Bind<Fz::Dog> : Class<Fz::Dog> {
    using Extends = Fz::Animal;
    static constexpr auto members = Alka::members(Alka::prop<"name", &Fz::Dog::mName>());
};

template <> struct Alka::Bind<Fz::Kennel> : Class<Fz::Kennel> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&Fz::Kennel::animal>(policy::internalRef),
                      Alka::method<&Fz::Kennel::putDog>(), Alka::method<&Fz::Kennel::adopt>(),
                      Alka::method<&Fz::Kennel::clear>(), Alka::method<&Fz::Kennel::chorus>());
};

template <> struct Alka::Bind<Fz::File> : Class<Fz::File> {
    static constexpr auto members = Alka::members(Alka::ctor<>(), Alka::method<&Fz::File::read>(),
                                                  Alka::disposable<&Fz::File::close>());
};

template <> struct Alka::Bind<Fz::Enemy> : Class<Fz::Enemy> {
    static constexpr auto members = Alka::members(Alka::prop<"hp", &Fz::Enemy::mHp>());
};

template <> struct Alka::Bind<Fz::Mod> : Module<"fz"> {
    static constexpr auto members = Alka::members(
        Alka::type<Fz::Point>, Alka::type<Fz::Polygon>, Alka::type<Fz::Animal>, Alka::type<Fz::Dog>,
        Alka::type<Fz::Kennel>, Alka::type<Fz::File>, Alka::type<Fz::Enemy>, Alka::fn<&Fz::spawn>(),
        Alka::fn<&Fz::killAll>(), Alka::fn<&Fz::share>(), Alka::fn<&Fz::dropShared>(),
        Alka::fn<&Fz::makePolygon>(), Alka::fn<&Fz::echo>(), Alka::fn<&Fz::total>(),
        Alka::fn<&Fz::range>(), Alka::fn<&Fz::call>());
};

namespace {

class Reader {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : mBytes(bytes) {}
    [[nodiscard]] bool done() const { return mAt >= mBytes.size(); }
    std::uint8_t byte() { return mAt < mBytes.size() ? mBytes[mAt++] : 0; }
    int slot() { return byte() & 7; }
    int number() { return static_cast<std::int8_t>(byte()); }

private:
    std::span<const std::uint8_t> mBytes;
    std::size_t mAt = 0;
};

auto statement(Reader& in) -> std::string {
    const int op = in.byte() % 34;
    const auto a = std::to_string(in.slot());
    const auto b = std::to_string(in.slot());
    const auto c = std::to_string(in.slot());
    const auto n = std::to_string(in.number());
    const auto v = [](const std::string& i) { return "v[" + i + "]"; };
    const auto set = [&](const std::string& i, const std::string& expression) {
        return v(i) + " = t(() => " + expression + ");";
    };
    const auto run = [](const std::string& expression) { return "t(() => " + expression + ");"; };
    switch (op) {
    case 0:
        return set(a, "new fz.Point(" + n + ")");
    case 1:
        return set(a, "new fz.Polygon()");
    case 2:
        return run(v(a) + ".add(" + n + ")");
    case 3:
        return set(a, v(b) + "[" + n + " & 7]");
    case 4:
        return run(v(a) + ".clear()");
    case 5:
        return run(v(a) + ".shrink()");
    case 6:
        return set(a, v(b) + ".x");
    case 7:
        return run("{ " + v(a) + ".x = " + v(b) + "; }");
    case 8:
        return set(a, v(b) + ".origin");
    case 9:
        return run("{ " + v(a) + ".origin = " + v(b) + "; }");
    case 10:
        return set(a, "[..." + v(b) + "]");
    case 11:
        return set(a, "fz.total(" + v(b) + ")");
    case 12:
        return set(a, (in.byte() & 1) ? "new Float64Array(" + n + " & 15)"
                                      : "[" + n + ", 1.5, " + v(b) + "]");
    case 13:
        return set(a, "fz.echo(" + v(b) + ")");
    case 14:
        return set(a, "fz.range(" + n + ")");
    case 15:
        return set(a, "fz.call((x) => { " + v(c) + " && " + v(c) + ".clear && " + v(c) +
                          ".clear(); return x + 1; }, " + n + ")");
    case 16:
        return set(a, "fz.share(" + v(b) + ")");
    case 17:
        return run("fz.dropShared()");
    case 18:
        return set(a, "fz.makePolygon(" + n + ")");
    case 19:
        return set(a, "new fz.Kennel()");
    case 20:
        return run(v(a) + ".putDog(" + n + ", 'rex')");
    case 21:
        return set(a, v(b) + ".animal(" + n + " & 15)");
    case 22:
        return set(a, "new (class extends fz.Animal { speak() { return String(" + v(b) + ") + " +
                          v(c) + "?.chorus?.(); } })()");
    case 23:
        return run(v(a) + ".adopt(" + v(b) + ")");
    case 24:
        return set(a, v(b) + ".chorus()");
    case 25:
        return set(a, "new fz.File()");
    case 26:
        return set(a, v(b) + ".read()");
    case 27:
        return run(v(a) + ".close()");
    case 28:
        return set(a, "fz.spawn()");
    case 29:
        return run("fz.killAll()");
    case 30:
        return run("gc()");
    case 31:
        return v(a) + " = " + v(b) + ";";
    case 32:
        return set(a, "JSON.stringify(" + v(b) + ")");
    default:
        return run("{ " + v(a) + ".tag = " + v(b) + "; " + v(c) + " = Object.keys(" + v(a) +
                   "); }");
    }
}

auto programOf(const std::span<const std::uint8_t> input) -> std::string {
    Reader in(input);
    std::string js = "(() => { const v = new Array(8);"
                     " const t = (f) => { try { return f(); } catch (e) { return e; } };\n";
    for (int i = 0; i < 64 && !in.done(); ++i) {
        js += statement(in);
        js += '\n';
    }
    js += "})(); undefined";
    return js;
}

struct Host {
    Alka::IsolateHandle mIsolate;
    std::optional<Alka::ContextScope> mScope;
    int mRuns = 0;

    void fresh() {
        mScope.reset();
        Fz::gShared.reset();
        Fz::gEnemies.clear();
        mIsolate = Alka::Engine::createIsolate(
            {.mMaxHeapBytes = std::size_t{256} << 20, .mOnError = [](std::string_view) {}});
        mScope.emplace(mIsolate.get(), Alka::ContextOptions{.mConsole = false});
        mScope->install<Fz::Mod>();
    }

    void run(const std::span<const std::uint8_t> input) {
        if (!mScope || ++mRuns % 128 == 0) {
            fresh(); // (the old isolate goes with live wrappers, views and all)
        }
        (void)mScope->eval(programOf(input), "<fuzz>", {.mTimeout = std::chrono::seconds(2)});
        (void)Alka::Engine::pump(mIsolate.get());
    }
};

Host& host() {
    static Host instance;
    return instance;
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    (void)Alka::Engine::instance({.mFlags = {"--expose-gc"}});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    host().run(std::span(data, size));
    return 0;
}

#ifndef ALKA_FUZZ_LIBFUZZER
int main(const int argc, char** argv) {
    (void)LLVMFuzzerInitialize(nullptr, nullptr);
    if (const std::string_view first = argc > 1 ? argv[1] : "";
        !first.empty() && first.find_first_not_of("0123456789") != std::string_view::npos) {
        for (int i = 1; i < argc; ++i) { // replay inputs
            std::ifstream file(argv[i], std::ios::binary);
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator(file), {}};
            (void)LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
            std::printf("replayed %s\n", argv[i]);
        }
        return 0;
    }
    const long runs = argc > 1 ? std::stol(argv[1]) : 2000;
    const auto seed = argc > 2 ? std::stoul(argv[2])
                               : static_cast<unsigned long>(
                                     std::chrono::steady_clock::now().time_since_epoch().count());
    std::printf("AlkaFuzz: %ld random inputs, seed %lu\n", runs, seed);
    std::mt19937 random(static_cast<std::mt19937::result_type>(seed));
    std::vector<std::uint8_t> bytes;
    for (long i = 0; i < runs; ++i) {
        bytes.resize(random() % 384);
        for (auto& b : bytes) {
            b = static_cast<std::uint8_t>(random());
        }
        (void)LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
        if ((i + 1) % 500 == 0) {
            std::printf("  %ld\n", i + 1);
            std::fflush(stdout);
        }
    }
    std::printf("ok\n");
    return 0;
}
#endif
