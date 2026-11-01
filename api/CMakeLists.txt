# The plugin interfaces: headers only, for the programs that load the plugins
# and for the plugins themselves.
add_library(spp-plugin-api INTERFACE)
add_library(siplasplas::plugin-api ALIAS spp-plugin-api)
target_include_directories(spp-plugin-api INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(spp-plugin-api INTERFACE Qt6::Core)
set_target_properties(spp-plugin-api PROPERTIES EXPORT_NAME plugin-api)

install(DIRECTORY siplasplas DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS spp-plugin-api EXPORT siplasplas-plugins-targets)
install(EXPORT siplasplas-plugins-targets
    NAMESPACE siplasplas::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/siplasplas-plugins)

include(CMakePackageConfigHelpers)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/siplasplas-plugins-config.cmake"
    "include(CMakeFindDependencyMacro)\n"
    "find_dependency(Qt6 COMPONENTS Core)\n"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/siplasplas-plugins-targets.cmake\")\n")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/siplasplas-plugins-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/siplasplas-plugins-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/siplasplas-plugins-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/siplasplas-plugins)
