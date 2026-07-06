# Shared warning/conformance flags for all Alka targets (library, tests, examples).
# Consumed as a PRIVATE link dependency so nothing leaks into user projects.

add_library(AlkaCompileOptions INTERFACE)
add_library(Alka::CompileOptions ALIAS AlkaCompileOptions)

target_compile_options(AlkaCompileOptions INTERFACE
  # MSVC cl
  "$<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4;/permissive-;/EHsc;/bigobj;/Zc:preprocessor;/Zc:__cplusplus;/external:W0>"
  # clang-cl (MSVC-compatible driver)
  "$<$<CXX_COMPILER_ID:Clang>:/utf-8;/W4;/EHsc;/bigobj>"
)
