# The find plugin: a bar searching and replacing in an spp::EditorView,
# through its interface only. Needs Qt Widgets, nothing else.
find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)
spp_add_plugin(spp-findbar FindPluginImpl.cpp FindBarWidget.cpp FindBarWidget.h)
target_link_libraries(spp-findbar PRIVATE Qt6::Widgets)
