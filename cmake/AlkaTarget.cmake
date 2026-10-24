function(alka_add_library)
    cmake_parse_arguments(ARG "ENGINE" "INCLUDE_DIR;SOURCE_DIR;V8" "" ${ARGN})
    set(sources
            Core/Error.cpp
            Core/Version.cpp
            Js/Value.cpp
            Runtime/Dispatcher.cpp
            Runtime/Errors.cpp
            Runtime/Gc.cpp
            Runtime/IsolateLink.cpp
            Runtime/Modules.cpp
            Runtime/Snapshot.cpp
            Runtime/State.cpp
            Runtime/Watchdog.cpp)
    if (ARG_ENGINE)
        list(APPEND sources
                Engine/Engine.cpp Engine/Inspector.cpp Engine/ModuleLoader.cpp Engine/WebGlobals.cpp)
    endif ()
    list(TRANSFORM sources PREPEND "${ARG_SOURCE_DIR}/")

    add_library(Alka STATIC ${sources})
    add_library(Alka::Alka ALIAS Alka)
    target_include_directories(Alka PUBLIC "${ARG_INCLUDE_DIR}")
    target_compile_features(Alka PUBLIC cxx_std_23)
    target_link_libraries(Alka PUBLIC ${ARG_V8})
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR MSVC)
        target_compile_options(Alka PUBLIC /utf-8 /EHsc /bigobj /permissive- /Zc:__cplusplus)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(Alka PUBLIC /Zc:preprocessor)
        endif ()
    endif ()
endfunction()
