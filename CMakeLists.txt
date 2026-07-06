cmake_minimum_required(VERSION 3.28)

project(Alka
  VERSION 0.1.0
  DESCRIPTION "Declarative C++23 <-> V8 binding library with automatic TypeScript definition generation"
  LANGUAGES CXX)

if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ALKA_BUILD_TESTS "Build Alka unit tests" ${PROJECT_IS_TOP_LEVEL})
option(ALKA_BUILD_EXAMPLES "Build Alka examples" ${PROJECT_IS_TOP_LEVEL})
# The self-hosted Engine/ContextScope layer bootstraps V8 itself (platform, ICU,
# V8::Initialize) and needs a V8 backend that ships libplatform. Embedded hosts
# (e.g. CloverNT) drive an existing isolate and must build with this OFF.
option(ALKA_BUILD_ENGINE "Build the self-hosted Engine/ContextScope layer (needs a V8 with libplatform)" ${PROJECT_IS_TOP_LEVEL})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AlkaCompileOptions)
include(V8Backend)

set(ALKA_SOURCES
  src/Error.cpp
  src/Install.cpp
  src/Version.cpp)
if(ALKA_BUILD_ENGINE)
  list(APPEND ALKA_SOURCES src/Engine.cpp)
endif()

add_library(Alka STATIC ${ALKA_SOURCES})
add_library(Alka::Alka ALIAS Alka)

target_include_directories(Alka PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(Alka
  PUBLIC ${ALKA_V8_TARGET}
  PRIVATE Alka::CompileOptions)

if(ALKA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ALKA_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
  add_subdirectory(examples)
endif()
