#pragma once

#include <Alka/Engine/Inspector.hpp>

#include <v8.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Serves an Alka::Inspector to Chrome DevTools (chrome://inspect) and VS Code
// over the DevTools WebSocket protocol.
//
//   Alka::Inspector inspector(isolate);
//   inspector.contextCreated(context, "main");
//   AlkaTools::InspectorBridge bridge(inspector, isolate);       // isolate thread
//   std::println("open {}", bridge.frontendUrl());
//   bridge.waitForFrontend();   // optional: until DevTools attached and is ready
//   bridge.pauseOnStart();      // optional: stop at the first statement
//   ... run scripts, draining the isolate's Alka::Dispatcher (Engine::pump does) ...

namespace AlkaTools {

class InspectorBridge {
public:
    InspectorBridge(Alka::Inspector& inspector, v8::Isolate* isolate, std::uint16_t port = 9229,
                    std::string title = "Alka");
    ~InspectorBridge();
    InspectorBridge(const InspectorBridge&) = delete;
    InspectorBridge& operator=(const InspectorBridge&) = delete;

    [[nodiscard]] bool listening() const noexcept;
    [[nodiscard]] auto port() const noexcept -> std::uint16_t;
    [[nodiscard]] auto webSocketUrl() const -> std::string;
    [[nodiscard]] auto frontendUrl() const -> std::string;
    [[nodiscard]] bool connected() const noexcept;

    void setPump(std::function<bool()> pump);
    bool waitForFrontend(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    void pauseOnStart();
    void serveUntilDetached();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace AlkaTools
