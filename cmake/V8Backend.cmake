# V8Backend.cmake -- BYO V8. Alka does NOT download or build V8; the host
# supplies it through a single CMake target named by ALKA_V8_TARGET.
#
# That target must carry, on its INTERFACE:
#   * the V8 headers            (INTERFACE_INCLUDE_DIRECTORIES -> <v8.h>)
#   * the V8 ABI macro set       (INTERFACE_COMPILE_DEFINITIONS)
#   * the V8 / Node import libs  (INTERFACE_LINK_LIBRARIES)
#
# Alka links it PUBLIC, so all three propagate to Alka's own TUs and to every
# consumer of Alka::Alka.
#
# Examples:
#   -DALKA_V8_TARGET=QQNT::QQNT     (CloverNT: V8 from the running QQ/Electron)
#   -DALKA_V8_TARGET=MyV8           (a self-built / prebuilt V8 imported target)
#
# ABI contract: every TU that includes <v8.h> must observe the exact same V8
# macro set (pointer compression / sandbox / 31-bit smis / ...) as the V8 build
# it ultimately links against. V8::Initialize() encodes the consumer's macro
# state and CHECK-fails at startup on mismatch. Because ALKA_V8_TARGET carries
# those macros PUBLIC-ly, anything that reaches <v8.h> through Alka is protected;
# never include <v8.h> without going through Alka::Alka (or ALKA_V8_TARGET).

set(ALKA_V8_TARGET "" CACHE STRING
    "CMake target providing the V8 backend (headers + ABI defines + link libs), e.g. QQNT::QQNT")
set(ALKA_V8_RUNTIME_DIR "" CACHE PATH
    "Optional: directory with a V8 runtime payload (DLLs + icudtl.dat) to stage next to self-hosted executables/tests")

if(NOT ALKA_V8_TARGET)
  message(FATAL_ERROR
      "Alka: no V8 backend configured.\n"
      "Alka no longer downloads V8 -- the host must provide it (BYO V8). Set "
      "ALKA_V8_TARGET to a target carrying the V8 headers, the matching ABI "
      "compile definitions, and the V8/Node import libraries, e.g.\n"
      "  -DALKA_V8_TARGET=QQNT::QQNT")
endif()

if(NOT TARGET ${ALKA_V8_TARGET})
  message(FATAL_ERROR
      "Alka: ALKA_V8_TARGET='${ALKA_V8_TARGET}' is not a CMake target. Define it "
      "before Alka is configured (i.e. add_subdirectory()/FetchContent Alka only "
      "after the target exists).")
endif()

message(STATUS "Alka: V8 backend target = ${ALKA_V8_TARGET}")

# Stages a host-provided V8 runtime (DLLs + icudtl.dat) next to a self-hosted
# executable/test. No-op unless ALKA_V8_RUNTIME_DIR is set -- embedded hosts
# (e.g. CloverNT) get V8 from the running process and need no staging, but the
# call sites in tests/examples must still resolve.
function(alka_copy_v8_runtime target)
  if(NOT ALKA_V8_RUNTIME_DIR)
    return()
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${ALKA_V8_RUNTIME_DIR}" "$<TARGET_FILE_DIR:${target}>"
    COMMENT "Alka: staging V8 runtime from ${ALKA_V8_RUNTIME_DIR} for ${target}"
    VERBATIM)
endfunction()
