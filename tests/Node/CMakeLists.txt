cmake_minimum_required(VERSION 3.28)

project(AlkaNodeAddonTest LANGUAGES CXX)

set(NODE_DIR "" CACHE PATH "A Node.js release (bin/node and include/node)")
set(NODE_EXTRA_INCLUDE "" CACHE PATH "Extra V8 headers for NODE_DIR's V8 (e.g. v8-fast-api-calls.h)")
if (NOT EXISTS "${NODE_DIR}/include/node/node.h")
    message(FATAL_ERROR "NODE_DIR must be a Node.js release directory (include/node/node.h)")
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

add_library(NodeV8 INTERFACE)
target_include_directories(NodeV8 SYSTEM INTERFACE "${NODE_DIR}/include/node")
target_compile_definitions(NodeV8 INTERFACE
        BUILDING_NODE_EXTENSION USING_V8_SHARED=1 USING_UV_SHARED=1)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT WIN32)
    target_compile_definitions(NodeV8 INTERFACE COMPONENT_BUILD)
endif ()
if (NODE_EXTRA_INCLUDE)
    target_include_directories(NodeV8 SYSTEM INTERFACE "${NODE_EXTRA_INCLUDE}")
    target_compile_definitions(NodeV8 INTERFACE ALKA_FAST_API_STATS=1)
endif ()
if (WIN32)
    target_link_libraries(NodeV8 INTERFACE "${NODE_DIR}/node.lib")
endif ()

set(ALKA_V8_TARGET NodeV8)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/alka")

add_library(alka_node_test MODULE Addon.cpp)
set_target_properties(alka_node_test PROPERTIES PREFIX "" SUFFIX ".node")
target_link_libraries(alka_node_test PRIVATE Alka::Alka Alka::CompileOptions)
if (APPLE)
    target_link_options(alka_node_test PRIVATE -undefined dynamic_lookup)
endif ()

enable_testing()
set(_node "${NODE_DIR}/bin/node")
if (WIN32)
    set(_node "${NODE_DIR}/node.exe")
endif ()
add_test(NAME NodeAddon
        COMMAND "${_node}" --expose-gc --allow-natives-syntax "${CMAKE_CURRENT_SOURCE_DIR}/test.js")
set_tests_properties(NodeAddon PROPERTIES
        ENVIRONMENT "ALKA_ADDON=$<TARGET_FILE:alka_node_test>" TIMEOUT 120)
