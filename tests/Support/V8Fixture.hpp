#pragma once

#include <gtest/gtest.h>

#include <Alka/Runtime/Dispatcher.hpp>
#include <Alka/Runtime/Install.hpp>

#include <libplatform/libplatform.h>
#include <v8.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace AlkaTest {

class V8Fixture {
public:
    V8Fixture() : mAllocator(v8::ArrayBuffer::Allocator::NewDefaultAllocator()) {
        v8::Isolate::CreateParams params;
        params.array_buffer_allocator = mAllocator.get();
        mIsolate = v8::Isolate::New(params);
        v8::Isolate::Scope isolateScope(mIsolate);
        v8::HandleScope handleScope(mIsolate);
        mContext.Reset(mIsolate, v8::Context::New(mIsolate));
    }

    V8Fixture(const V8Fixture&) = delete;
    V8Fixture& operator=(const V8Fixture&) = delete;

    ~V8Fixture() {
        mContext.Reset();
        Alka::disposeIsolate(mIsolate);
        mIsolate->Dispose();
    }

    [[nodiscard]] v8::Isolate* isolate() const noexcept { return mIsolate; }

    template <class Fn> decltype(auto) run(Fn&& fn) {
        v8::Isolate::Scope isolateScope(mIsolate);
        v8::HandleScope handleScope(mIsolate);
        v8::Local<v8::Context> context = mContext.Get(mIsolate);
        v8::Context::Scope contextScope(context);
        return std::forward<Fn>(fn)(mIsolate, context);
    }

    void collectGarbage() {
        run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
            for (int i = 0; i < 2; ++i) {
                v8::Local<v8::Value> ignored;
                std::ignore = compileAndRun(isolate, context, "gc()").ToLocal(&ignored);
            }
        });
    }

    static v8::MaybeLocal<v8::Value>
    compileAndRun(v8::Isolate* isolate, v8::Local<v8::Context> context, std::string_view source) {
        v8::Local<v8::String> src =
            v8::String::NewFromUtf8(isolate, source.data(), v8::NewStringType::kNormal,
                                    static_cast<int>(source.size()))
                .ToLocalChecked();
        v8::Local<v8::Script> script;
        if (!v8::Script::Compile(context, src).ToLocal(&script)) {
            return {};
        }
        return script->Run(context);
    }

    std::int32_t evalInt(std::string_view source) { return evalAs<std::int32_t>(source); }
    double evalDouble(std::string_view source) { return evalAs<double>(source); }
    bool evalBool(std::string_view source) { return evalAs<bool>(source); }

    template <class Pred> bool pumpUntil(Pred done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            run([](v8::Isolate* isolate, v8::Local<v8::Context>) {
                (void)Alka::Dispatcher(isolate).drain();
                isolate->PerformMicrotaskCheckpoint();
            });
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    std::string evalString(std::string_view source) {
        return run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) -> std::string {
            v8::TryCatch tryCatch(isolate);
            v8::Local<v8::Value> value;
            if (!compileAndRun(isolate, context, source).ToLocal(&value)) {
                failFromTryCatch(isolate, context, tryCatch, source);
                return {};
            }
            v8::String::Utf8Value utf8(isolate, value);
            return *utf8 ? std::string(*utf8, static_cast<std::size_t>(utf8.length()))
                         : std::string();
        });
    }

private:
    template <class T> T evalAs(std::string_view source) {
        return run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) -> T {
            v8::TryCatch tryCatch(isolate);
            v8::Local<v8::Value> value;
            if (!compileAndRun(isolate, context, source).ToLocal(&value)) {
                failFromTryCatch(isolate, context, tryCatch, source);
                return T{};
            }
            if constexpr (std::is_same_v<T, bool>) {
                return value->BooleanValue(isolate);
            } else if constexpr (std::is_same_v<T, std::int32_t>) {
                return value->Int32Value(context).FromMaybe(0);
            } else {
                return static_cast<T>(value->NumberValue(context).FromMaybe(0.0));
            }
        });
    }

    static void failFromTryCatch(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                 v8::TryCatch& tryCatch, std::string_view source) {
        std::string message = "<no exception info>";
        if (tryCatch.HasCaught()) {
            v8::String::Utf8Value text(isolate, tryCatch.Exception());
            if (*text) {
                message.assign(*text, static_cast<std::size_t>(text.length()));
            }
            v8::Local<v8::Value> trace;
            if (tryCatch.StackTrace(context).ToLocal(&trace)) {
                v8::String::Utf8Value traceText(isolate, trace);
                if (*traceText) {
                    message += '\n';
                    message.append(*traceText, static_cast<std::size_t>(traceText.length()));
                }
            }
        }
        ADD_FAILURE() << "JS threw while evaluating \"" << std::string(source) << "\": " << message;
    }

    std::unique_ptr<v8::ArrayBuffer::Allocator> mAllocator;
    v8::Isolate* mIsolate = nullptr;
    v8::Global<v8::Context> mContext;
};

} // namespace AlkaTest
