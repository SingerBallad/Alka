#pragma once

#include <Alka/Core/Error.hpp>
#include <Alka/Js/Value.hpp>
#include <Alka/Runtime/Dispatcher.hpp>
#include <Alka/Runtime/Install.hpp>
#include <Alka/Runtime/Modules.hpp>

#include <v8.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// auto& engine = Alka::Engine::instance();
// auto isolate = engine.createIsolate({.mMaxHeapBytes = 64 << 20});
// Alka::ContextScope scope(isolate.get());
// scope.install<Game>();
// auto result = scope.runModuleFile("main.mjs", {.mTimeout = std::chrono::seconds(5)});
// Alka::Engine::runUntilIdle(isolate.get());     // timers, promises, coroutines

namespace Alka {

struct EngineOptions {
    std::string mIcuDataPath{};        // empty: look for icudtl.dat next to the executable
    int mThreadPoolSize = 0;           // 0: platform default
    std::vector<std::string> mFlags{}; // e.g. "--expose-gc"
    v8::Platform* mPlatform = nullptr;
    bool mDefaultPlatform = true;
};

using ErrorReporter = std::function<void(std::string_view message)>;

struct IsolateOptions {
    std::size_t mMaxHeapBytes = 0;
    ErrorReporter mOnError{};
    bool mCppHeap = false;
};

enum class ConsoleLevel : std::uint8_t { Log, Info, Debug, Warn, Error };
using ConsoleSink = std::function<void(ConsoleLevel level, std::string_view text)>;

struct ContextOptions {
    bool mConsole = true;       // console.* (formatted like Node's)
    ConsoleSink mConsoleSink{}; // default: log/info/debug to stdout, warn/error to stderr
    bool mTimers = true;        // setTimeout / setInterval / setImmediate / clear*
    bool mWebGlobals = true;
    std::filesystem::path mModuleRoot{};
};

struct EvalOptions {
    std::chrono::milliseconds mTimeout{0};
};

class IsolateHandle {
public:
    IsolateHandle() = default;
    IsolateHandle(v8::Isolate* isolate, std::unique_ptr<v8::ArrayBuffer::Allocator> allocator)
        : mIsolate(isolate), mAllocator(std::move(allocator)) {}
    IsolateHandle(IsolateHandle&& other) noexcept
        : mIsolate(other.mIsolate), mAllocator(std::move(other.mAllocator)) {
        other.mIsolate = nullptr;
    }
    IsolateHandle& operator=(IsolateHandle&& other) noexcept {
        if (this != &other) {
            reset();
            mIsolate = other.mIsolate;
            mAllocator = std::move(other.mAllocator);
            other.mIsolate = nullptr;
        }
        return *this;
    }
    IsolateHandle(const IsolateHandle&) = delete;
    IsolateHandle& operator=(const IsolateHandle&) = delete;
    ~IsolateHandle() { reset(); }

    [[nodiscard]] auto get() const noexcept -> v8::Isolate* { return mIsolate; }
    [[nodiscard]] auto operator->() const noexcept -> v8::Isolate* { return mIsolate; }
    explicit operator bool() const noexcept { return mIsolate != nullptr; }

private:
    void reset();

    v8::Isolate* mIsolate = nullptr;
    std::unique_ptr<v8::ArrayBuffer::Allocator> mAllocator;
};

class Engine {
public:
    static Engine& instance(const EngineOptions& options = {});

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] static IsolateHandle createIsolate(const IsolateOptions& options = {});

    static bool pump(v8::Isolate* isolate);

    static bool runUntilIdle(v8::Isolate* isolate);

private:
    friend class IsolateHandle;
    explicit Engine(const EngineOptions& options);
    ~Engine();

    std::unique_ptr<v8::Platform> mOwnedPlatform; // null: the host's V8
    v8::Platform* mPlatform = nullptr;
    bool mPumpsPlatform = true;
};

class ContextScope {
public:
    explicit ContextScope(v8::Isolate* isolate, const ContextOptions& options = {});
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
    ~ContextScope();

    [[nodiscard]] auto isolate() const noexcept -> v8::Isolate* { return mIsolate; }
    [[nodiscard]] auto context() const -> v8::Local<v8::Context> { return mContext; }

    template <BoundModule... Mods> auto install() const -> void {
        Alka::install<Mods...>(mContext);
    }

    template <BoundModule... Mods> auto registerModules() const -> void {
        Alka::registerModules<Mods...>(mContext);
    }

    [[nodiscard]] Result<Value> eval(std::string_view source,
                                     std::string_view scriptName = "<eval>",
                                     const EvalOptions& options = {}) const;

    [[nodiscard]] Result<Value> evalModule(std::string_view source,
                                           std::string_view moduleName = "<module>",
                                           const EvalOptions& options = {}) const;

    [[nodiscard]] Result<Value> runModuleFile(const std::filesystem::path& path,
                                              const EvalOptions& options = {}) const;

    [[nodiscard]] Result<std::string> evalToString(std::string_view source,
                                                   std::string_view scriptName = "<eval>",
                                                   const EvalOptions& options = {}) const;

private:
    v8::Isolate* mIsolate = nullptr;
    v8::Isolate::Scope mIsolateScope;
    v8::HandleScope mHandleScope;
    v8::Local<v8::Context> mContext;
    v8::Context::Scope mContextScope;
};

} // namespace Alka
