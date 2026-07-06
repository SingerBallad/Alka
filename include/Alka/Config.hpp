#pragma once

// Compile-time knobs. Override with target_compile_definitions before
// including any Alka header.

// Default value range scanned when reflecting enum entries. The range is
// additionally clamped to the enum's underlying type. Enums outside this
// range (e.g. large flag values) can specialize Alka::EnumRange<E> instead.
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

// Context embedder-data slot used for Alka's per-context data (wrapper
// identity cache). Change it when your host already uses this slot.
#ifndef ALKA_CONTEXT_EMBEDDER_SLOT
#define ALKA_CONTEXT_EMBEDDER_SLOT 1
#endif
