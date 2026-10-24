#pragma once

#include <v8.h>

#include <functional>
#include <memory>
#include <string_view>

// Alka::Inspector inspector(isolate);
// inspector.contextCreated(context, "main");
// auto session = inspector.connect([&](std::string_view json) { socket.send(json); });
// ...  session->dispatch(incomingJson);   // isolate thread (post through a Dispatcher)
//
// While paused at a breakpoint V8 blocks inside the inspector; the pause pump
// (default: Engine::pump + Dispatcher drain) keeps delivering messages posted
// to the isolate's Dispatcher, so a transport thread can send "Debugger.resume".
// A ready-made WebSocket transport for Chrome DevTools / VS Code lives in
// tools/InspectorBridge.

namespace Alka {

class InspectorSession {
public:
    InspectorSession(const InspectorSession&) = delete;
    InspectorSession& operator=(const InspectorSession&) = delete;
    ~InspectorSession();

    void dispatch(std::string_view message);

    void schedulePauseOnNextStatement();

private:
    friend class Inspector;
    struct Impl;
    explicit InspectorSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> mImpl;
};

class Inspector {
public:
    explicit Inspector(v8::Isolate* isolate);
    Inspector(const Inspector&) = delete;
    Inspector& operator=(const Inspector&) = delete;
    ~Inspector();

    void contextCreated(v8::Local<v8::Context> context, std::string_view name);
    void contextDestroyed(v8::Local<v8::Context> context);

    [[nodiscard]] auto connect(std::function<void(std::string_view)> send)
        -> std::unique_ptr<InspectorSession>;

    void setPausePump(std::function<void()> pump);

    void onFrontendReady(std::function<void()> ready);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Alka
