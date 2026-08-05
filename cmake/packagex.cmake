# Packaging + install helpers moved here so deb/cmake packaging logic can be reused.
# This file expects to be included from the project root CMakeLists.txt where
# PROJECT_VERSION and GNUInstallDirs variables are already defined.

# install library and export targets
install(TARGETS slotsboxmalloc
    EXPORT slotsboxmallocTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# install public headers
install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/../include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Export targets and generate CMake config package
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/slotsboxmallocConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/SlotsboxmallocConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/slotsboxmallocConfig.cmake"
    @ONLY
)

install(EXPORT slotsboxmallocTargets
    FILE slotsboxmallocTargets.cmake
    NAMESPACE slotsboxmalloc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slotsboxmalloc-${PROJECT_VERSION}
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/slotsboxmallocConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/slotsboxmallocConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slotsboxmalloc-${PROJECT_VERSION})
