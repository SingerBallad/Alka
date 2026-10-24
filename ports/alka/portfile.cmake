if (DEFINED ENV{ALKA_SOURCE_DIR})
    set(SOURCE_PATH "$ENV{ALKA_SOURCE_DIR}")
else ()
    get_filename_component(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif ()

set(VCPKG_BUILD_TYPE release)

vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}"
        OPTIONS
        -DALKA_PACKAGE_ONLY=ON
        -DALKA_BUILD_TESTS=OFF
        -DALKA_BUILD_EXAMPLES=OFF
        -DALKA_BUILD_BENCHMARKS=OFF
        -DALKA_BUILD_ENGINE=OFF)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME alka CONFIG_PATH lib/cmake/Alka)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib" "${CURRENT_PACKAGES_DIR}/share/doc")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
