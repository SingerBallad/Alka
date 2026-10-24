#pragma once

#include <Alka/Engine/Engine.hpp>

#include <v8.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Alka::EngineDetail {

using Clock = std::chrono::steady_clock;

struct Timer {
    Clock::time_point mDue;
    std::chrono::milliseconds mInterval{0};
    bool mRepeat = false;
    bool mRef = true; // keeps runUntilIdle waiting
    v8::Global<v8::Function> mCallback;
    std::vector<v8::Global<v8::Value>> mArgs;
    v8::Global<v8::Context> mContext;
};

struct ModuleRecord {
    v8::Global<v8::Module> mModule;
    std::string mPath; // absolute file path, or empty
    std::string mUrl;  // what import.meta.url says
    v8::Global<v8::Value> mJson;
};

struct ContextState {
    v8::Global<v8::Context> mContext;
    std::filesystem::path mModuleRoot;
    std::vector<std::unique_ptr<ModuleRecord>> mModules;
};

struct WakeSignal {
    std::mutex mMutex;
    std::condition_variable mWake;
    bool mWoken = false;

    void wake() {
        {
            const std::lock_guard lock(mMutex);
            mWoken = true;
        }
        mWake.notify_all();
    }
};

struct IsolateState {
    v8::Isolate* mIsolate = nullptr;
    ErrorReporter mOnError;
    std::size_t mMaxHeapBytes = 0;
    bool mHeapLimitHit = false; // set inside V8's near-heap-limit callback
    bool mErrorReported = false;

    std::map<std::uint32_t, Timer> mTimers;
    std::uint32_t mNextTimerId = 1;
    std::vector<std::pair<v8::Global<v8::Promise>, v8::Global<v8::Value>>> mRejections;
    std::vector<std::unique_ptr<ContextState>> mContexts;

    std::shared_ptr<WakeSignal> mWakeSignal = std::make_shared<WakeSignal>();
    bool mHasWakeup = false; // the Dispatcher wakes mWakeSignal

    void report(std::string_view message);
};

[[nodiscard]] auto stateOf(const v8::Isolate* isolate) -> IsolateState*;
[[nodiscard]] auto ensureState(v8::Isolate* isolate) -> IsolateState&;
void forgetState(const v8::Isolate* isolate);

[[nodiscard]] auto contextStateOf(IsolateState& state, v8::Local<v8::Context> context)
    -> ContextState*;
[[nodiscard]] auto ensureContextState(IsolateState& state, v8::Local<v8::Context> context,
                                      const std::filesystem::path& root) -> ContextState&;
void forgetContextState(IsolateState& state, v8::Local<v8::Context> context);

[[nodiscard]] auto describeError(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                 v8::Local<v8::Value> error) -> std::string;

void installTimers(v8::Local<v8::Context> context, IsolateState& state);
bool runDueTimers(IsolateState& state);
[[nodiscard]] auto nextTimerDue(const IsolateState& state) -> std::optional<Clock::time_point>;
[[nodiscard]] bool hasRefTimers(const IsolateState& state);

void installConsole(v8::Local<v8::Context> context, ConsoleSink sink);
void installWebGlobals(v8::Local<v8::Context> context);

void installModuleHooks(v8::Isolate* isolate);
auto resolveFileModule(v8::Local<v8::Context> context, v8::Local<v8::String> specifier,
                       v8::Local<v8::FixedArray> attributes, v8::Local<v8::Module> referrer)
    -> v8::MaybeLocal<v8::Module>;
[[nodiscard]] auto compileModule(ContextState& state, v8::Local<v8::Context> context,
                                 std::string_view source, std::string_view name,
                                 const std::string& path) -> v8::MaybeLocal<v8::Module>;

} // namespace Alka::EngineDetail
