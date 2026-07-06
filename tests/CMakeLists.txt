include(FetchContent)

# doctest's own CMakeLists still declares an old minimum version; keep CMake 4.x happy.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
  URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70)
FetchContent_MakeAvailable(doctest)

add_executable(AlkaTests
  TestMain.cpp
  TestBindForms.cpp
  TestClasses.cpp
  TestConvertContainers.cpp
  TestConvertFunctional.cpp
  TestConvertPrimitives.cpp
  TestEnum.cpp
  TestFunctions.cpp
  TestInheritance.cpp
  TestLifetime.cpp
  TestMeta.cpp
  TestMultiContext.cpp
  TestNameOf.cpp
  TestSmoke.cpp
  TestTsGen.cpp)

target_link_libraries(AlkaTests PRIVATE
  Alka::Alka
  doctest::doctest
  Alka::CompileOptions)
target_include_directories(AlkaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# Real std headers instead of doctest's forward declarations, which MSVC's
# C++23 mode rejects with C5285.
target_compile_definitions(AlkaTests PRIVATE
  DOCTEST_CONFIG_USE_STD_HEADERS
  ALKA_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Golden")

alka_copy_v8_runtime(AlkaTests)

add_test(NAME AlkaTests COMMAND AlkaTests)
