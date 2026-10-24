#include "EngineState.hpp"

#include <Alka/Convert/Functional.hpp>
#include <Alka/Runtime/Watchdog.hpp>

#include <libplatform/libplatform.h>
#if ALKA_HAS_CPPGC
#include <v8-cppgc.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace Alka {

namespace EngineDetail {

namespace {

std::string executablePath() {
#if defined(_WIN32)
    std::string path(MAX_PATH, '\0');
    while (true) {
        const auto length =
            GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path.resize(std::char_traits<char>::length(path.c_str()));
    return path;
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::string{} : path.string();
#endif
}

auto statesMutex() -> std::mutex& {
    static auto& mutex = *new std::mutex;
    return mutex;
}
auto states() -> std::map<const v8::Isolate*, std::unique_ptr<IsolateState>>& {
    static auto& registry = *new std::map<const v8::Isolate*, std::unique_ptr<IsolateState>>;
    return registry;
}

} // namespace

void IsolateState::report(const std::string_view message) {
    mErrorReported = true;
    if (mOnError) {
        mOnError(message);
        return;
    }
    std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

auto stateOf(const v8::Isolate* isolate) -> IsolateState* {
    const std::lock_guard lock(statesMutex());
    const auto it = states().find(isolate);
    return it != states().end() ? it->second.get() : nullptr;
}

auto ensureState(v8::Isolate* isolate) -> IsolateState& {
    {
        const std::lock_guard lock(statesMutex());
        if (const auto it = states().find(isolate); it != states().end()) {
            return *it->second;
        }
    }
    auto made = std::make_unique<IsolateState>();
    made->mIsolate = isolate;
    auto& state = *made;
    {
        const std::lock_guard lock(statesMutex());
        states().emplace(isolate, std::move(made));
    }
    onDisposeIsolate(isolate, [isolate] { forgetState(isolate); });
    return state;
}

void forgetState(const v8::Isolate* isolate) {
    std::unique_ptr<IsolateState> gone;
    {
        const std::lock_guard lock(statesMutex());
        if (const auto it = states().find(isolate); it != states().end()) {
            gone = std::move(it->second);
            states().erase(it);
        }
    }
    // (its handles are released here, while the isolate is alive)
}

auto contextStateOf(IsolateState& state, const v8::Local<v8::Context> context) -> ContextState* {
    for (const auto& each : state.mContexts) {
        if (each->mContext == context) {
            return each.get();
        }
    }
    return nullptr;
}

auto ensureContextState(IsolateState& state, const v8::Local<v8::Context> context,
                        const std::filesystem::path& root) -> ContextState& {
    if (auto* existing = contextStateOf(state, context)) {
        return *existing;
    }
    auto made = std::make_unique<ContextState>();
    made->mContext.Reset(state.mIsolate, context);
    made->mModuleRoot = root;
    state.mContexts.push_back(std::move(made));
    return *state.mContexts.back();
}

void forgetContextState(IsolateState& state, const v8::Local<v8::Context> context) {
    std::erase_if(state.mContexts, [&](const auto& each) { return each->mContext == context; });
}

auto describeError(v8::Isolate* isolate, const v8::Local<v8::Context> context,
                   const v8::Local<v8::Value> error) -> std::string {
    const v8::TryCatch tryCatch(isolate);
    if (error->IsObject()) {
        v8::Local<v8::Value> stack;
        if (error.As<v8::Object>()
                ->Get(context, Detail::cachedKey<"stack">(isolate))
                .ToLocal(&stack) &&
            stack->IsString()) {
            return Detail::stringToUtf8(isolate, stack.As<v8::String>());
        }
    }
    v8::Local<v8::String> text;
    if (!error->IsSymbol() && error->ToString(context).ToLocal(&text)) {
        return Detail::stringToUtf8(isolate, text);
    }
    return "<unprintable value>";
}

namespace {

auto addTimer(IsolateState& state, const CallInfo& call, const Value& callback,
              const Rest<Value>& rest, const bool repeat) -> double {
    const auto isolate = call.mIsolate;
    const auto fn = callback.local(isolate);
    if (fn.IsEmpty() || !fn->IsFunction()) {
        Detail::throwTypeError(isolate, "the timer callback must be a function");
        return 0;
    }
    double delay = 0;
    if (!rest.empty()) {
        if (const auto value = rest.front().local(isolate);
            !value.IsEmpty() && !value->NumberValue(call.mContext).To(&delay)) {
            return 0; // (valueOf threw: no timer, the exception propagates)
        }
        if (!std::isfinite(delay) || delay < 0) {
            delay = 0;
        }
    }
    Timer timer;
    const auto interval =
        std::chrono::milliseconds(static_cast<std::int64_t>(std::min(delay, 2.0e9)));
    timer.mInterval = interval;
    timer.mRepeat = repeat;
    timer.mDue = Clock::now() + interval;
    timer.mCallback.Reset(isolate, fn.As<v8::Function>());
    for (std::size_t i = 1; i < rest.size(); ++i) {
        timer.mArgs.emplace_back(isolate, rest[i].local(isolate));
    }
    timer.mContext.Reset(isolate, call.mContext);
    const auto id = state.mNextTimerId++;
    state.mTimers.emplace(id, std::move(timer));
    return id;
}

void clearTimer(IsolateState& state, const CallInfo& call, const Value& id) {
    if (const auto value = id.local(call.mIsolate); !value.IsEmpty() && value->IsNumber()) {
        state.mTimers.erase(static_cast<std::uint32_t>(value.As<v8::Number>()->Value()));
    }
}

} // namespace

void installTimers(const v8::Local<v8::Context> context, IsolateState& state) {
    ConvertCtx cx{state.mIsolate, context};
    const auto global = Object::global(cx);
    auto* const target = &state;
    std::ignore = global.set(
        "setTimeout",
        callable(
            [target](const CallInfo& call, const Value& callback, const Rest<Value>& rest) {
                return addTimer(*target, call, callback, rest, false);
            },
            "setTimeout"));
    std::ignore = global.set(
        "setInterval",
        callable(
            [target](const CallInfo& call, const Value& callback, const Rest<Value>& rest) {
                return addTimer(*target, call, callback, rest, true);
            },
            "setInterval"));
    std::ignore =
        global.set("setImmediate",
                   callable(
                       [target](const CallInfo& call, const Value& callback, Rest<Value> rest) {
                           rest.insert(rest.begin(), Value()); // (no delay)
                           return addTimer(*target, call, callback, rest, false);
                       },
                       "setImmediate"));
    for (const char* name : {"clearTimeout", "clearInterval", "clearImmediate"}) {
        std::ignore =
            global.set(name, callable([target](const CallInfo& call,
                                               const Value& id) { clearTimer(*target, call, id); },
                                      name));
    }
}

bool hasRefTimers(const IsolateState& state) {
    return std::ranges::any_of(state.mTimers, [](const auto& entry) { return entry.second.mRef; });
}

auto nextTimerDue(const IsolateState& state) -> std::optional<Clock::time_point> {
    std::optional<Clock::time_point> next;
    for (const auto& [id, timer] : state.mTimers) {
        if (!next || timer.mDue < *next) {
            next = timer.mDue;
        }
    }
    return next;
}

bool runDueTimers(IsolateState& state, const Clock::time_point now) {
    const auto isolate = state.mIsolate;
    bool ran = false;
    std::vector<std::uint32_t> due;
    for (const auto& [id, timer] : state.mTimers) {
        if (timer.mDue <= now) {
            due.push_back(id);
        }
    }
    std::ranges::sort(due, [&](const std::uint32_t a, const std::uint32_t b) {
        const auto& ta = state.mTimers.at(a);
        const auto& tb = state.mTimers.at(b);
        return ta.mDue != tb.mDue ? ta.mDue < tb.mDue : a < b;
    });
    for (const auto id : due) {
        const auto it = state.mTimers.find(id);
        if (it == state.mTimers.end()) {
            continue;
        }
        const v8::HandleScope scope(isolate);
        auto& timer = it->second;
        const auto context = timer.mContext.Get(isolate);
        const auto callback = timer.mCallback.Get(isolate);
        std::vector<v8::Local<v8::Value>> args;
        for (const auto& arg : timer.mArgs) {
            args.push_back(arg.Get(isolate));
        }
        if (timer.mRepeat) {
            timer.mDue = Clock::now() + std::max(timer.mInterval, std::chrono::milliseconds(1));
        } else {
            state.mTimers.erase(it);
        }
        if (context.IsEmpty()) {
            continue;
        }
        const v8::Context::Scope contextScope(context);
        v8::TryCatch tryCatch(isolate);
        if (callback
                ->Call(context, v8::Undefined(isolate), static_cast<int>(args.size()), args.data())
                .IsEmpty()) {
            if (tryCatch.HasTerminated()) {
                isolate->CancelTerminateExecution();
                state.mHeapLimitHit = false;
                state.report("a timer callback was terminated");
            } else if (tryCatch.HasCaught()) {
                state.report("Uncaught " + describeError(isolate, context, tryCatch.Exception()));
            }
        }
        isolate->PerformMicrotaskCheckpoint();
        ran = true;
    }
    return ran;
}

bool runDueTimers(IsolateState& state) {
    return runDueTimers(state, Clock::now());
}

namespace {

void promiseRejected(v8::PromiseRejectMessage message) {
    const auto promise = message.GetPromise();
    const auto isolate = v8::Isolate::GetCurrent();
    auto* state = stateOf(isolate);
    if (state == nullptr) {
        return;
    }
    switch (message.GetEvent()) {
    case v8::kPromiseRejectWithNoHandler:
        state->mRejections.emplace_back(v8::Global<v8::Promise>(isolate, promise),
                                        v8::Global<v8::Value>(isolate, message.GetValue()));
        break;
    case v8::kPromiseHandlerAddedAfterReject:
        std::erase_if(state->mRejections,
                      [&](const auto& entry) { return entry.first == promise; });
        break;
    default:
        break;
    }
}

void reportRejections(IsolateState& state) {
    if (state.mRejections.empty()) {
        return;
    }
    const auto isolate = state.mIsolate;
    const v8::HandleScope scope(isolate);
    auto pending = std::move(state.mRejections);
    state.mRejections.clear();
    for (const auto& [promise, reason] : pending) {
        const auto local = promise.Get(isolate);
        v8::Local<v8::Context> context;
        if (!Detail::creationContextOf(isolate, local).ToLocal(&context)) {
            context = isolate->GetCurrentContext();
        }
        if (context.IsEmpty()) {
            continue;
        }
        const v8::Context::Scope contextScope(context);
        state.report("Uncaught (in promise) " +
                     describeError(isolate, context, reason.Get(isolate)));
    }
}

std::size_t nearHeapLimit(void* data, const std::size_t current, const std::size_t initial) {
    auto& state = *static_cast<IsolateState*>(data);
    state.mHeapLimitHit = true;
    state.mIsolate->TerminateExecution();
    state.mIsolate->AutomaticallyRestoreInitialHeapLimit(0.5);
    return current + (std::max)(initial / 2, std::size_t{16} << 20);
}

void prepareIsolate(v8::Isolate* isolate) {
    isolate->SetPromiseRejectCallback(&promiseRejected);
    installModuleHooks(isolate);
}

class DrainTask final : public v8::Task {
public:
    explicit DrainTask(Dispatcher dispatcher) : mDispatcher(std::move(dispatcher)) {}
    void Run() override { (void)mDispatcher.drain(); }

private:
    Dispatcher mDispatcher;
};

} // namespace

} // namespace EngineDetail

using namespace EngineDetail;

void IsolateHandle::reset() {
    if (mIsolate != nullptr) {
        disposeIsolate(mIsolate);
        if (auto& engine = Engine::instance(); engine.mPumpsPlatform) {
            v8::platform::NotifyIsolateShutdown(engine.mPlatform, mIsolate);
        }
        mIsolate->Dispose();
        mIsolate = nullptr;
    }
    mAllocator.reset();
}

Engine& Engine::instance(const EngineOptions& options) {
    static Engine engine(options);
    return engine;
}

IsolateHandle Engine::createIsolate(const IsolateOptions& options) {
    auto* const platform = instance().mPlatform;
    auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator.get();
    if (options.mMaxHeapBytes > 0) {
        params.constraints.ConfigureDefaultsFromHeapSize(0, options.mMaxHeapBytes);
    }
    if (options.mCppHeap) {
#if ALKA_HAS_CPPGC
        // (V8 owns a CppHeap handed over this way)
        params.cpp_heap = v8::CppHeap::Create(platform, v8::CppHeapCreateParams({})).release();
#else
        throw JsException("Alka: this V8 offers no C++ heap for Alka::GcObject (V8 13+)");
#endif
    }
    auto isolate = v8::Isolate::New(params);
    auto& state = ensureState(isolate);
    state.mOnError = options.mOnError;
    state.mMaxHeapBytes = options.mMaxHeapBytes;
    state.mHasWakeup = true;
    {
        const v8::Isolate::Scope isolateScope(isolate);
        const Dispatcher dispatcher(isolate);
        dispatcher.setWakeup([platform, isolate, dispatcher, signal = state.mWakeSignal] {
            if (const auto runner =
                    platform->GetForegroundTaskRunner(isolate, v8::TaskPriority::kUserBlocking)) {
                runner->PostTask(std::make_unique<DrainTask>(dispatcher));
            }
            signal->wake();
        });
        dispatcher.setActivityHook([signal = state.mWakeSignal] { signal->wake(); });
        if (options.mMaxHeapBytes > 0) {
            isolate->AddNearHeapLimitCallback(&nearHeapLimit, &state);
            onDisposeIsolate(
                isolate, [isolate] { isolate->RemoveNearHeapLimitCallback(&nearHeapLimit, 0); });
        }
        prepareIsolate(isolate);
    }
    return {isolate, std::move(allocator)};
}

bool Engine::pump(v8::Isolate* isolate) {
    const auto& engine = instance();
    auto* state = stateOf(isolate);
    const auto now = Clock::now();
    bool any = state != nullptr && runDueTimers(*state, now);
    while (true) {
        bool ran = false;
        while (engine.mPumpsPlatform && v8::platform::PumpMessageLoop(engine.mPlatform, isolate)) {
            ran = true;
        }
        if (Dispatcher(isolate).drain() > 0) {
            ran = true;
        }
        isolate->PerformMicrotaskCheckpoint();
        if (state != nullptr) {
            reportRejections(*state);
        }
        if (!ran) {
            return any;
        }
        any = true;
    }
}

bool Engine::runUntilIdle(v8::Isolate* isolate) {
    auto& state = ensureState(isolate);
    const Dispatcher dispatcher(isolate);
    while (true) {
        if (pump(isolate)) {
            continue;
        }
        const auto next = nextTimerDue(state);
        if (!dispatcher.busy() && !hasRefTimers(state)) {
            return !std::exchange(state.mErrorReported, false);
        }
        auto until = Clock::now() + (state.mHasWakeup ? std::chrono::milliseconds(100)
                                                      : std::chrono::milliseconds(1));
        if (next && *next < until) {
            until = *next;
        }
        auto& signal = *state.mWakeSignal;
        std::unique_lock lock(signal.mMutex);
        signal.mWake.wait_until(lock, until, [&] { return signal.mWoken; });
        signal.mWoken = false;
    }
}

Engine::Engine(const EngineOptions& options) {
    if (options.mPlatform != nullptr) {
        mPlatform = options.mPlatform;
        mPumpsPlatform = options.mDefaultPlatform;
        return;
    }
    for (const auto& flag : options.mFlags) {
        v8::V8::SetFlagsFromString(flag.c_str(), flag.size());
    }
    if (options.mIcuDataPath.empty()) {
        const auto exe = executablePath();
        v8::V8::InitializeICUDefaultLocation(exe.c_str());
    } else {
        v8::V8::InitializeICU(options.mIcuDataPath.c_str());
    }
    mOwnedPlatform = v8::platform::NewDefaultPlatform(options.mThreadPoolSize);
    mPlatform = mOwnedPlatform.get();
    v8::V8::InitializePlatform(mPlatform);
    v8::V8::Initialize();
}

Engine::~Engine() {
    if (mOwnedPlatform) {
        v8::V8::Dispose();
        v8::V8::DisposePlatform();
    }
}

namespace {

void defaultConsoleSink(const ConsoleLevel level, const std::string_view text) {
    auto* const out = level == ConsoleLevel::Warn || level == ConsoleLevel::Error ? stderr : stdout;
    std::fwrite(text.data(), 1, text.size(), out);
    std::fputc('\n', out);
    std::fflush(out);
}

auto runFailure(v8::Isolate* isolate, const v8::Local<v8::Context> context, v8::TryCatch& tryCatch,
                const Watchdog* watchdog, const EvalOptions& options) -> ConvertError {
    if (tryCatch.HasTerminated()) {
        if (watchdog != nullptr && watchdog->fired()) {
            return ConvertError{"the script timed out after " +
                                std::to_string(options.mTimeout.count()) + " ms"};
        }
        if (auto* state = stateOf(isolate); state != nullptr && state->mHeapLimitHit) {
            state->mHeapLimitHit = false;
            isolate->CancelTerminateExecution();
            return ConvertError{"JS heap limit reached (" +
                                std::to_string(state->mMaxHeapBytes >> 20) + " MB)"};
        }
    }
    return Detail::caughtError(isolate, context, tryCatch);
}

bool isFilePath(const std::string_view name) {
    return !name.empty() && name.front() != '<' &&
           (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos ||
            name.ends_with(".js") || name.ends_with(".mjs"));
}

} // namespace

ContextScope::ContextScope(v8::Isolate* isolate, const ContextOptions& options)
    : mIsolate(isolate), mIsolateScope(isolate), mHandleScope(isolate),
      mContext(v8::Context::New(isolate)), mContextScope(mContext) {
    auto& state = ensureState(isolate);
    std::error_code error;
    auto root =
        options.mModuleRoot.empty() ? std::filesystem::current_path(error) : options.mModuleRoot;
    (void)ensureContextState(state, mContext, std::filesystem::absolute(root, error));
    prepareIsolate(isolate);
    if (options.mConsole) {
        installConsole(mContext, options.mConsoleSink ? options.mConsoleSink : &defaultConsoleSink);
    }
    if (options.mTimers) {
        installTimers(mContext, state);
    }
    if (options.mWebGlobals) {
        installWebGlobals(mContext);
    }
}

ContextScope::~ContextScope() {
    if (auto* state = stateOf(mIsolate)) {
        std::erase_if(state->mTimers,
                      [&](const auto& entry) { return entry.second.mContext == mContext; });
        forgetContextState(*state, mContext);
    }
}

auto ContextScope::eval(const std::string_view source, const std::string_view scriptName,
                        const EvalOptions& options) const -> Result<Value> {
    std::optional<Watchdog> watchdog;
    if (options.mTimeout.count() > 0) {
        watchdog.emplace(mIsolate, options.mTimeout);
    }
    v8::TryCatch tryCatch(mIsolate);
    v8::ScriptOrigin origin(Detail::v8Str(mIsolate, scriptName));
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(mContext, Detail::v8Str(mIsolate, source), &origin).ToLocal(&script)) {
        return std::unexpected(runFailure(mIsolate, mContext, tryCatch, nullptr, options));
    }
    v8::Local<v8::Value> value;
    if (!script->Run(mContext).ToLocal(&value)) {
        return std::unexpected(
            runFailure(mIsolate, mContext, tryCatch, watchdog ? &*watchdog : nullptr, options));
    }
    ConvertCtx cx{mIsolate, mContext};
    return Value(cx, value);
}

auto ContextScope::evalModule(const std::string_view source, const std::string_view moduleName,
                              const EvalOptions& options) const -> Result<Value> {
    auto& state = ensureState(mIsolate);
    std::error_code error;
    auto& context = ensureContextState(state, mContext, std::filesystem::current_path(error));
    std::string path;
    if (isFilePath(moduleName)) {
        const auto file = std::filesystem::weakly_canonical(
            std::filesystem::absolute(
                std::filesystem::path(std::u8string(moduleName.begin(), moduleName.end())), error),
            error);
        const auto utf8 = file.u8string();
        path.assign(utf8.begin(), utf8.end());
    }
    std::optional<Watchdog> watchdog;
    if (options.mTimeout.count() > 0) {
        watchdog.emplace(mIsolate, options.mTimeout);
    }
    const Watchdog* const limit = watchdog ? &*watchdog : nullptr;
    v8::TryCatch tryCatch(mIsolate);
    v8::Local<v8::Module> module;
    if (!compileModule(context, mContext, source, moduleName, path).ToLocal(&module) ||
        !module->InstantiateModule(mContext, &resolveFileModule).FromMaybe(false)) {
        return std::unexpected(runFailure(mIsolate, mContext, tryCatch, limit, options));
    }
    v8::Local<v8::Value> completion;
    if (!module->Evaluate(mContext).ToLocal(&completion)) {
        return std::unexpected(runFailure(mIsolate, mContext, tryCatch, limit, options));
    }
    if (completion->IsPromise()) {
        const auto promise = completion.As<v8::Promise>();
        if (promise->State() == v8::Promise::kPending) {
            mIsolate->PerformMicrotaskCheckpoint();
        }
        if (promise->State() == v8::Promise::kRejected) {
            promise->MarkAsHandled();
            std::erase_if(state.mRejections,
                          [&](const auto& entry) { return entry.first == promise; });
            const v8::String::Utf8Value text(mIsolate, promise->Result());
            return std::unexpected(ConvertError{
                *text != nullptr ? std::string(*text, static_cast<std::size_t>(text.length()))
                                 : std::string("module evaluation failed"),
                Detail::ValueState::make(mIsolate, mContext, promise->Result())});
        }
    }
    ConvertCtx cx{mIsolate, mContext};
    return Value(cx, module->GetModuleNamespace());
}

auto ContextScope::runModuleFile(const std::filesystem::path& path,
                                 const EvalOptions& options) const -> Result<Value> {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(ConvertError{"cannot read " + path.string()});
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::error_code error;
    const auto absolute =
        std::filesystem::weakly_canonical(std::filesystem::absolute(path, error), error);
    const auto name = absolute.u8string();
    return evalModule(buffer.str(),
                      std::string_view(reinterpret_cast<const char*>(name.data()), name.size()),
                      options);
}

auto ContextScope::evalToString(const std::string_view source, const std::string_view scriptName,
                                const EvalOptions& options) const -> Result<std::string> {
    auto value = eval(source, scriptName, options);
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    if (!value->valid()) {
        return std::string{};
    }
    const auto local = value->local(mIsolate);
    v8::Local<v8::String> text;
    if (!local->ToString(mContext).ToLocal(&text)) {
        return std::string("<unprintable>");
    }
    return Detail::stringToUtf8(mIsolate, text);
}

} // namespace Alka
