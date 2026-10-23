vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ufbx/ufbx
    REF 19bdb7e7ef02eb914d5e7211a3685f50ee6d27e3
    SHA512 66d42b62ed3e55c19cb99cd55876853bf48ce3b09b026fa54d1719e78b40ccdfd2ca44d3410fb9552f60e6d5cdf9e5d911ee4164d905250af87f90011dbab1a4
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
     DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/ufbx-config.cmake.in"
     DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME ufbx CONFIG_PATH lib/cmake/ufbx)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
