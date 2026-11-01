find_package(GTest REQUIRED)
include(GoogleTest)

if(TARGET spp-archive)
    # The archive plugin, loaded from its file as programs load it.
    add_executable(archive_plugin_tests test_archive_plugin.cpp)
    target_compile_definitions(archive_plugin_tests PRIVATE
        SPP_ARCHIVE_PLUGIN_FILE="$<TARGET_FILE:spp-archive>")
    target_include_directories(archive_plugin_tests PRIVATE ${SPP_LIBARCHIVE_INCLUDE_DIRS})
    target_link_libraries(archive_plugin_tests PRIVATE
        siplasplas::plugin-api Qt6::Core GTest::gtest ${SPP_LIBARCHIVE_LIBRARIES})
    add_dependencies(archive_plugin_tests spp-archive)
    gtest_discover_tests(archive_plugin_tests)
endif()
