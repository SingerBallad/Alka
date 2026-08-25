include(CheckCXXCompilerFlag)

add_library(AlkaCompileOptions INTERFACE)
add_library(Alka::CompileOptions ALIAS AlkaCompileOptions)

function(alka_target_supported_options target scope)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    string(APPEND CMAKE_REQUIRED_FLAGS " -Werror=unused-command-line-argument")
  endif()
  foreach(flag IN LISTS ARGN)
    string(MAKE_C_IDENTIFIER "ALKA_HAVE${flag}" have_var)
    check_cxx_compiler_flag("${flag}" ${have_var})
    if(${have_var})
      target_compile_options(${target} ${scope} "${flag}")
    endif()
  endforeach()
endfunction()

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR MSVC)
  target_compile_options(AlkaCompileOptions INTERFACE
    /utf-8 /W4 /EHsc /bigobj)
  alka_target_supported_options(AlkaCompileOptions INTERFACE
    /permissive- /Zc:preprocessor /Zc:__cplusplus /external:W0)
else()
  target_compile_options(AlkaCompileOptions INTERFACE
    -Wall -Wextra)
  if(WIN32)
    alka_target_supported_options(AlkaCompileOptions INTERFACE -Wa,-mbig-obj)
  endif()
endif()
