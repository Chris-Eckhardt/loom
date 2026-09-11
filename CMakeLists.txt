cmake_minimum_required(VERSION 3.14)
project(loom VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

# --- loom library ---------------------------------------------------------
add_library(loom
    src/cpu_affinity.cpp
    src/fiber.cpp
    src/job_system.cpp)
add_library(loom::loom ALIAS loom)
target_include_directories(loom PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(loom PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(loom PUBLIC Threads::Threads)

# --- install --------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(TARGETS loom
    EXPORT loomTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    src/job.h
    src/loom.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/loom
)

install(EXPORT loomTargets
    FILE loomTargets.cmake
    NAMESPACE loom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/loomConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
)

# --- examples -------------------------------------------------------------
option(LOOM_BUILD_EXAMPLES "Build the loom examples" ON)
if(LOOM_BUILD_EXAMPLES)
    add_executable(loom_example examples/basic_example.cpp)
    target_link_libraries(loom_example PRIVATE loom::loom)
endif()

# --- tests ----------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(loom_default_tests ON)
else()
    set(loom_default_tests OFF)
endif()
option(LOOM_BUILD_TESTS "Build loom's test suite" ${loom_default_tests})

if(LOOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()