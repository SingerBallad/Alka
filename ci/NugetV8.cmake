# V8 13.0 for Windows from pmed/v8-nuget's packages (v8-v143-x64 headers and
# import libraries, v8.redist-v143-x64 DLLs), unpacked into
# ALKA_V8_NUGET_DIR; ALKA_V8_NUGET_CONFIG picks Debug or Release.
#   cmake -DALKA_V8_TARGET=NugetV8 -DCMAKE_PROJECT_INCLUDE=ci/NugetV8.cmake
#         -DALKA_V8_NUGET_DIR=<dir> -DALKA_V8_RUNTIME_DIR=<redist dir> ...
# Its DLLs export no inline members: never define USING_V8_SHARED with it.
if(NOT TARGET NugetV8)
  if(NOT ALKA_V8_NUGET_CONFIG)
    set(ALKA_V8_NUGET_CONFIG Release)
  endif()
  set(_lib "${ALKA_V8_NUGET_DIR}/lib/${ALKA_V8_NUGET_CONFIG}")
  set(_defs V8_31BIT_SMIS_ON_64BIT_ARCH V8_COMPRESS_POINTERS V8_ENABLE_SANDBOX)
  if(ALKA_V8_NUGET_CONFIG STREQUAL "Debug")
    list(APPEND _defs V8_ENABLE_CHECKS)  # (the Debug build checks; headers must agree)
  endif()
  add_library(NugetV8 INTERFACE IMPORTED GLOBAL)
  set_target_properties(NugetV8 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ALKA_V8_NUGET_DIR}/include"
    INTERFACE_COMPILE_DEFINITIONS "${_defs}"
    INTERFACE_LINK_LIBRARIES
      "${_lib}/v8.dll.lib;${_lib}/v8_libbase.dll.lib;${_lib}/v8_libplatform.dll.lib;dbghelp.lib;shlwapi.lib;winmm.lib")
endif()
