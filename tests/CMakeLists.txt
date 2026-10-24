include(FetchContent)

# Not GoogleTest's own CMake project: it enables C, and next to clang-cl CMake picks a C compiler
# that cannot configure. Its two amalgamated sources build with the project's flags instead.
FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
        URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
        SOURCE_SUBDIR googletest/include)
FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)
add_library(AlkaGTest STATIC
        "${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc"
        "${googletest_SOURCE_DIR}/googlemock/src/gmock-all.cc")
target_include_directories(AlkaGTest SYSTEM PUBLIC
        "${googletest_SOURCE_DIR}/googletest/include"
        "${googletest_SOURCE_DIR}/googlemock/include")
target_include_directories(AlkaGTest PRIVATE
        "${googletest_SOURCE_DIR}/googletest"
        "${googletest_SOURCE_DIR}/googlemock")
target_link_libraries(AlkaGTest PUBLIC Threads::Threads)

add_executable(AlkaTests
        TestMain.cpp
        Core/TestNameOf.cpp
        Binding/TestBindForms.cpp
        Binding/TestDynamicMembers.cpp
        Binding/TestIdioms.cpp
        Binding/TestRecords.cpp
        Binding/TestEnum.cpp
        Binding/TestMeta.cpp
        Binding/TestNaming.cpp
        Binding/TestOverride.cpp
        Convert/TestContainers.cpp
        Convert/TestFunctional.cpp
        Convert/TestPrimitives.cpp
        Convert/TestTypedArrays.cpp
        Js/TestAsync.cpp
        Js/TestCallables.cpp
        Js/TestDynamic.cpp
        Js/TestHandles.cpp
        Runtime/TestClasses.cpp
        Runtime/TestConcurrency.cpp
        Runtime/TestDispose.cpp
        Runtime/TestEngine.cpp
        Runtime/TestErrors.cpp
        Runtime/TestFastApi.cpp
        Runtime/TestFunctions.cpp
        Runtime/TestGc.cpp
        Runtime/TestInspector.cpp
        Runtime/TestInspectorBridge.cpp
        Runtime/BenchHotPaths.cpp
        Runtime/TestInheritance.cpp
        Runtime/TestLifetime.cpp
        Runtime/TestLifetimeTools.cpp
        Runtime/TestLivePaths.cpp
        Runtime/TestModules.cpp
        Runtime/TestMultiContext.cpp
        Runtime/TestOwnership.cpp
        Runtime/TestParams.cpp
        Runtime/TestPolicies.cpp
        Runtime/TestReviewRegressions.cpp
        Runtime/TestSmoke.cpp
        Runtime/TestSnapshot.cpp
        Runtime/TestStrictBorrow.cpp
        Ts/TestTsGen.cpp)

target_link_libraries(AlkaTests PRIVATE
        Alka::Alka
        Alka::InspectorBridge
        AlkaGTest
        Alka::CompileOptions)
target_include_directories(AlkaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(AlkaTests PRIVATE "-Wno-#warnings")
endif ()

target_compile_definitions(AlkaTests PRIVATE
        ALKA_FAST_API_STATS=1
        ALKA_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Ts/Golden")

alka_copy_v8_runtime(AlkaTests)

add_test(NAME AlkaTests COMMAND AlkaTests)
