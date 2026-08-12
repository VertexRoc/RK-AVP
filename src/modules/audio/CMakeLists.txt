add_library(rkavp_audio SHARED
  backend.cpp
  nodes.cpp
  plugin.cpp
)
add_library(RKAVP::Audio ALIAS rkavp_audio)
target_include_directories(rkavp_audio PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rkavp_audio PUBLIC RKAVP::Core PRIVATE RKAVP::ALSA)
set_target_properties(rkavp_audio PROPERTIES EXPORT_NAME Audio)
