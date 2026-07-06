#include <Alka/Engine.hpp>

#include <libplatform/libplatform.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Alka {

namespace {

std::string executablePath() {
#ifdef _WIN32
    char buffer[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    return buffer;
#else
    return {};
#endif
}

}  // namespace

Engine& Engine::instance(const EngineOptions& options) {
    static Engine engine(options);
    return engine;
}

IsolateHandle Engine::createIsolate() {
    auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator.get();
    auto isolate = v8::Isolate::New(params);
    return {isolate, std::move(allocator)};
}

Engine::Engine(const EngineOptions& options) {
    for (const auto& flag : options.mFlags) {
        v8::V8::SetFlagsFromString(flag.c_str(), flag.size());
    }
    if (options.mIcuDataPath.empty()) {
        const auto exe = executablePath();
        v8::V8::InitializeICUDefaultLocation(exe.c_str());
    } else {
        v8::V8::InitializeICU(options.mIcuDataPath.c_str());
    }
    mPlatform = v8::platform::NewDefaultPlatform(options.mThreadPoolSize);
    v8::V8::InitializePlatform(mPlatform.get());
    v8::V8::Initialize();
}

Engine::~Engine() {
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
}

ContextScope::ContextScope(v8::Isolate* isolate)
    : mIsolate(isolate), mIsolateScope(isolate), mHandleScope(isolate),
      mContext(v8::Context::New(isolate)), mContextScope(mContext) {}

ContextScope::~ContextScope() = default;

auto ContextScope::eval(const std::string_view source, const std::string_view scriptName) const
    -> Result<Value> {
    const v8::TryCatch tryCatch(mIsolate);
    v8::ScriptOrigin origin(Detail::v8Str(mIsolate, scriptName));
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(mContext, Detail::v8Str(mIsolate, source), &origin).ToLocal(&script)) {
        return std::unexpected(ConvertError{Detail::tryCatchMessage(mIsolate, mContext, tryCatch)});
    }
    v8::Local<v8::Value> value;
    if (!script->Run(mContext).ToLocal(&value)) {
        return std::unexpected(ConvertError{Detail::tryCatchMessage(mIsolate, mContext, tryCatch)});
    }
    const ConvertCtx cx{mIsolate, mContext};
    return Value(cx, value);
}

auto ContextScope::evalToString(const std::string_view source,
                                const std::string_view scriptName) const -> Result<std::string> {
    auto value = eval(source, scriptName);
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
    const v8::String::Utf8Value utf8(mIsolate, text);
    return *utf8 != nullptr ? std::string(*utf8, static_cast<std::size_t>(utf8.length()))
                            : std::string{};
}

}  // namespace Alka
