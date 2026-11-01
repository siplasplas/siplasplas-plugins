find_package(GTest REQUIRED)
include(GoogleTest)

if(TARGET spp-archive)
    # The archive plugin, loaded from its file as programs load it.
    # ExternalTools: its parsers are checked directly.
    add_executable(archive_plugin_tests test_archive_plugin.cpp test_external_tools.cpp
        ${PROJECT_SOURCE_DIR}/archive/ExternalTools.cpp)
    target_compile_definitions(archive_plugin_tests PRIVATE
        SPP_ARCHIVE_PLUGIN_FILE="$<TARGET_FILE:spp-archive>")
    target_include_directories(archive_plugin_tests PRIVATE ${SPP_LIBARCHIVE_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/archive)
    target_link_libraries(archive_plugin_tests PRIVATE
        siplasplas::plugin-api Qt6::Core GTest::gtest ${SPP_LIBARCHIVE_LIBRARIES})
    add_dependencies(archive_plugin_tests spp-archive)
    gtest_discover_tests(archive_plugin_tests)
endif()

if(TARGET spp-editor)
    # The editor plugin, loaded from its file, on the offscreen platform.
    find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)
    add_executable(editor_plugin_tests test_editor_plugin.cpp)
    target_compile_definitions(editor_plugin_tests PRIVATE
        SPP_EDITOR_PLUGIN_FILE="$<TARGET_FILE:spp-editor>")
    target_link_libraries(editor_plugin_tests PRIVATE
        siplasplas::plugin-api Qt6::Widgets GTest::gtest)
    add_dependencies(editor_plugin_tests spp-editor)
    gtest_discover_tests(editor_plugin_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
