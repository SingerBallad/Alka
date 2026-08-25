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
