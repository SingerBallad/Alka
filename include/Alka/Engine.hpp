#pragma once

#include <Alka/Error.hpp>
#include <Alka/Function.hpp>
#include <Alka/Install.hpp>

#include <v8.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Alka {

struct EngineOptions {
    std::string mIcuDataPath;        // empty: look for icudtl.dat next to the executable
    int mThreadPoolSize = 0;         // 0: platform default
    std::vector<std::string> mFlags; // e.g. "--expose-gc"
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
    void reset() {
        if (mIsolate != nullptr) {
            disposeIsolate(mIsolate);
            mIsolate->Dispose();
            mIsolate = nullptr;
        }
        mAllocator.reset();
    }

    v8::Isolate* mIsolate = nullptr;
    std::unique_ptr<v8::ArrayBuffer::Allocator> mAllocator;
};

class Engine {
public:
    static Engine& instance(const EngineOptions& options = {});

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] static IsolateHandle createIsolate();

private:
    explicit Engine(const EngineOptions& options);
    ~Engine();

    std::unique_ptr<v8::Platform> mPlatform;
};

class ContextScope {
public:
    explicit ContextScope(v8::Isolate* isolate);
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
    ~ContextScope();

    [[nodiscard]] auto isolate() const noexcept -> v8::Isolate* { return mIsolate; }
    [[nodiscard]] auto context() const -> v8::Local<v8::Context> { return mContext; }

    template <BoundModule... Mods> auto install(const InstallOptions& options = {}) const -> void {
        Alka::install<Mods...>(mContext, options);
    }

    Result<Value> eval(std::string_view source, std::string_view scriptName = "<eval>") const;
    Result<std::string> evalToString(std::string_view source,
                                     std::string_view scriptName = "<eval>") const;

private:
    v8::Isolate* mIsolate = nullptr;
    v8::Isolate::Scope mIsolateScope;
    v8::HandleScope mHandleScope;
    v8::Local<v8::Context> mContext;
    v8::Context::Scope mContextScope;
};

} // namespace Alka
