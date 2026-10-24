#pragma once

#define ALKA_VERSION_MAJOR 0
#define ALKA_VERSION_MINOR 2
#define ALKA_VERSION_PATCH 0
#define ALKA_VERSION_STRING "0.2.0"

#ifndef ALKA_ENUM_RANGE_MIN
#define ALKA_ENUM_RANGE_MIN (-128)
#endif
#ifndef ALKA_ENUM_RANGE_MAX
#define ALKA_ENUM_RANGE_MAX 128
#endif

static_assert(ALKA_ENUM_RANGE_MIN <= 0 && ALKA_ENUM_RANGE_MAX >= 0,
              "Alka enum reflection range must include 0");
static_assert(ALKA_ENUM_RANGE_MAX - ALKA_ENUM_RANGE_MIN <= 4096,
              "Alka enum reflection range is unreasonably large (compile-time cost)");

#ifndef ALKA_INT64_MODE
#define ALKA_INT64_MODE 0
#endif
static_assert(ALKA_INT64_MODE >= 0 && ALKA_INT64_MODE <= 2,
              "Alka: ALKA_INT64_MODE must be 0, 1 or 2");

#ifndef ALKA_FAST_API
#if __has_include(<v8-fast-api-calls.h>)
#define ALKA_FAST_API 1
#else
#define ALKA_FAST_API 0
#endif
#endif
#ifndef ALKA_FAST_API_STATS
#define ALKA_FAST_API_STATS 0
#endif

#ifndef ALKA_STRICT_BORROW
#define ALKA_STRICT_BORROW 1
#endif

#ifndef ALKA_HAS_CPPGC
#if __has_include(<v8-cppgc.h>) && __has_include(<cppgc/garbage-collected.h>) && \
    __has_include(<v8-version.h>)
#include <v8-version.h>
#if V8_MAJOR_VERSION >= 13 && (!defined(_WIN32) || defined(USING_V8_SHARED))
#define ALKA_HAS_CPPGC 1
#endif
#endif
#endif
#ifndef ALKA_HAS_CPPGC
#define ALKA_HAS_CPPGC 0
#endif
#ifndef ALKA_CONTEXT_EMBEDDER_SLOT
#define ALKA_CONTEXT_EMBEDDER_SLOT 64
#endif
#ifndef ALKA_CONTEXT_EMBEDDER_PROBES
#define ALKA_CONTEXT_EMBEDDER_PROBES 8
#endif

#ifndef ALKA_EMBEDDER_TAG_TYPE_NODE
#define ALKA_EMBEDDER_TAG_TYPE_NODE 12
#endif
#ifndef ALKA_EMBEDDER_TAG_HOLDER
#define ALKA_EMBEDDER_TAG_HOLDER 13
#endif
#ifndef ALKA_EMBEDDER_TAG_CONTEXT
#define ALKA_EMBEDDER_TAG_CONTEXT 14
#endif

namespace Alka {

[[nodiscard]] const char* versionString() noexcept;

} // namespace Alka
