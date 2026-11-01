cmake_minimum_required(VERSION 3.21)

project(siplasplas-plugins VERSION 0.2.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# Without a build type the compiler gets no optimization: a plain "cmake .."
# builds Release. A parent project (FetchContent) decides for itself.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type: Release, Debug, RelWithDebInfo or MinSizeRel" FORCE)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core)
find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)

# Where the plugins go. A program that fetches this repository (FetchContent)
# may set these before, e.g. to its own plugin directory; by default they are
# built into <build>/plugins and installed to lib/siplasplas/qt6/plugins.
if(NOT DEFINED SPP_PLUGIN_BUILD_DIR)
    set(SPP_PLUGIN_BUILD_DIR "${CMAKE_BINARY_DIR}/plugins")
endif()
if(NOT DEFINED SPP_PLUGIN_INSTALL_DIR)
    set(SPP_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/siplasplas/qt6/plugins")
endif()

option(SPP_BUILD_ARCHIVE "Build the archive plugin (needs libarchive)" ON)
# Tests (GoogleTest) when built on its own, not when fetched by a program.
option(SPP_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
# The interface headers and CMake package are installed when built on its
# own; a program fetching the repository installs only the plugins.
option(SPP_INSTALL_API "Install the interface headers and CMake package" ${PROJECT_IS_TOP_LEVEL})

add_subdirectory(api)

# A plugin module: built into SPP_PLUGIN_BUILD_DIR, installed to
# SPP_PLUGIN_INSTALL_DIR.
function(spp_add_plugin target)
    add_library(${target} MODULE ${ARGN})
    set_target_properties(${target} PROPERTIES
        AUTOMOC ON
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${SPP_PLUGIN_BUILD_DIR}")
    target_link_libraries(${target} PRIVATE siplasplas::plugin-api Qt6::Core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS ${target} LIBRARY DESTINATION ${SPP_PLUGIN_INSTALL_DIR})
endfunction()

if(SPP_BUILD_ARCHIVE)
    pkg_check_modules(SPP_LIBARCHIVE libarchive)
    if(SPP_LIBARCHIVE_FOUND)
        add_subdirectory(archive)
    else()
        message(STATUS "libarchive not found: no archive plugin")
    endif()
endif()

if(SPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
