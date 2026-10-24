#pragma once

#include <v8.h>

#include <chrono>
#include <memory>

// A time limit for JS: once `limit` passes, the isolate's running JS is
// terminated (from a watchdog thread).
//
//   {
//       Alka::Watchdog watchdog(isolate, std::chrono::milliseconds(500));
//       auto result = script->Run(context);      // empty when terminated
//       if (watchdog.fired()) { ... "timed out" ... }
//   }

namespace Alka {

class Watchdog {
public:
    Watchdog(v8::Isolate* isolate, std::chrono::milliseconds limit);
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    ~Watchdog();

    [[nodiscard]] bool fired() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Alka
