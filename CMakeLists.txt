cmake_minimum_required(VERSION 3.28)

project(Alka
        VERSION 0.2.0
        DESCRIPTION "Declarative C++23 <-> V8 binding library with automatic TypeScript definition generation"
        LANGUAGES CXX)

if (NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
endif ()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (NOT DEFINED CMAKE_CXX_SCAN_FOR_MODULES)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif ()

option(ALKA_BUILD_TESTS "Build Alka unit tests" ${PROJECT_IS_TOP_LEVEL})
option(ALKA_BUILD_EXAMPLES "Build Alka examples" ${PROJECT_IS_TOP_LEVEL})
option(ALKA_BUILD_ENGINE "Build the self-hosted Engine/ContextScope layer (needs a V8 with libplatform)" ${PROJECT_IS_TOP_LEVEL})
option(ALKA_BUILD_BENCHMARKS "Build AlkaBench (Alka next to hand-written V8 bindings; needs the Engine layer)" ${PROJECT_IS_TOP_LEVEL})
option(ALKA_BUILD_FUZZERS "Build AlkaFuzz (needs the Engine layer)" OFF)
option(ALKA_INSTALL "Install Alka as a CMake package (compiled in the consumer, against its V8)" ${PROJECT_IS_TOP_LEVEL})
option(ALKA_PACKAGE_ONLY "Only install the source package (no V8, nothing built)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AlkaTarget)
if (ALKA_PACKAGE_ONLY)
    set(ALKA_INSTALL ON)
else ()
    include(AlkaCompileOptions)
    include(V8Backend)

    set(_alka_engine "")
    if (ALKA_BUILD_ENGINE)
        set(_alka_engine ENGINE)
    endif ()
    alka_add_library(
            INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src"
            V8 ${ALKA_V8_TARGET}
            ${_alka_engine})
    target_link_libraries(Alka PRIVATE Alka::CompileOptions)
endif ()

if (ALKA_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(ALKA_INSTALL_SOURCEDIR "${CMAKE_INSTALL_DATADIR}/Alka/src")
    set(_alka_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/Alka")
    install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(DIRECTORY src/ DESTINATION "${ALKA_INSTALL_SOURCEDIR}")
    configure_package_config_file(cmake/AlkaConfig.cmake.in
            "${CMAKE_CURRENT_BINARY_DIR}/AlkaConfig.cmake"
            INSTALL_DESTINATION "${_alka_cmakedir}"
            PATH_VARS CMAKE_INSTALL_INCLUDEDIR ALKA_INSTALL_SOURCEDIR)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/AlkaConfigVersion.cmake"
            COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/AlkaConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/AlkaConfigVersion.cmake"
            cmake/AlkaTarget.cmake
            DESTINATION "${_alka_cmakedir}")
    install(FILES LICENSE README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Alka")
endif ()

if (ALKA_PACKAGE_ONLY)
    return()
endif ()

if (ALKA_BUILD_ENGINE)
    add_subdirectory(tools)
endif ()

if (ALKA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (ALKA_BUILD_BENCHMARKS AND ALKA_BUILD_ENGINE)
    add_subdirectory(benchmarks)
endif ()

if (ALKA_BUILD_FUZZERS AND ALKA_BUILD_ENGINE)
    add_subdirectory(fuzz)
endif ()

if (ALKA_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif ()
