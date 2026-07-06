#pragma once

#define ALKA_VERSION_MAJOR 0
#define ALKA_VERSION_MINOR 1
#define ALKA_VERSION_PATCH 0
#define ALKA_VERSION_STRING "0.1.0"

namespace Alka {

template<class T>
struct Bind;

[[nodiscard]] const char* versionString() noexcept;

}  // namespace Alka
