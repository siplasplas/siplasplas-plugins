# The editor plugin: qcodeedit's editor widget (github.com/siplasplas/qcodeedit,
# LGPL-3.0-or-later) with encodings (qcodeedit-encoding on cpg), Kate syntax
# colours (qcodeedit-kate) and reloads as an undo step (qcodeedit-diff). No
# menus, spell checking nor encrypted files: the program brings its own.
#
# qcodeedit is fetched and built here with position-independent code, which
# a plugin needs and an installed static cpg may lack; only the parts the
# plugin uses. FETCHCONTENT_SOURCE_DIR_QCODEEDIT may name a local checkout.
find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)
include(FetchContent)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(QCE_BUILD_DEMO OFF)
set(QCE_BUILD_TESTS OFF)
set(QCE_BUILD_KATE ON)
set(QCE_BUILD_KATE_DOWNLOADER OFF)
set(QCE_BUILD_ENCODING ON)
set(QCE_BUILD_GRAMMAR OFF)
set(QCE_BUILD_SPELL OFF)
set(QCE_BUILD_SPELL_DOWNLOADER OFF)
set(QCE_BUILD_DIFF ON)
set(CMAKE_DISABLE_FIND_PACKAGE_cpg TRUE)
FetchContent_Declare(qcodeedit
    GIT_REPOSITORY https://github.com/siplasplas/qcodeedit.git
    GIT_TAG v1.70.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(qcodeedit)

spp_add_plugin(spp-editor EditorPluginImpl.cpp EditorWidget.cpp EditorWidget.h
    BreakpointGutter.cpp BreakpointGutter.h
    CompletionPopup.cpp CompletionPopup.h)
target_link_libraries(spp-editor PRIVATE
    Qt6::Widgets
    qcodeedit::qcodeedit
    qcodeedit::kate
    qcodeedit::encoding
    qcodeedit::diff
    ${CMAKE_DL_LIBS})
# qcodeedit's headers need C++20.
target_compile_features(spp-editor PRIVATE cxx_std_20)

# Encoding detection reads cpg's languages.txt and n-gram models at run time:
# the plugin uses the copy installed next to it (spp-editor/cpg), else the
# place they had when it was built.
install(FILES "${CPG_LANGUAGES_FILE}" DESTINATION "${SPP_PLUGIN_INSTALL_DIR}/spp-editor/cpg")
install(DIRECTORY "${CPG_MODELS_DIR}/" DESTINATION "${SPP_PLUGIN_INSTALL_DIR}/spp-editor/cpg/models")
