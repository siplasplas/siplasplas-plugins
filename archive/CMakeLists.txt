# The archive plugin on libarchive, with lsar, 7z and unar as its reserve.
spp_add_plugin(spp-archive LibarchivePlugin.cpp ExternalTools.cpp ExternalTools.h)
target_include_directories(spp-archive PRIVATE ${SPP_LIBARCHIVE_INCLUDE_DIRS})
target_link_libraries(spp-archive PRIVATE ${SPP_LIBARCHIVE_LIBRARIES})
