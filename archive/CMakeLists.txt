# The archive plugin on libarchive.
spp_add_plugin(spp-archive LibarchivePlugin.cpp)
target_include_directories(spp-archive PRIVATE ${SPP_LIBARCHIVE_INCLUDE_DIRS})
target_link_libraries(spp-archive PRIVATE ${SPP_LIBARCHIVE_LIBRARIES})
