add_library(rkavp_streaming SHARED
  backend.cpp
  nodes.cpp
  plugin.cpp
)
add_library(RKAVP::Streaming ALIAS rkavp_streaming)
target_include_directories(rkavp_streaming PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rkavp_streaming
  PUBLIC RKAVP::Core
  PRIVATE RKAVP::ZLMediaKit
)
set_target_properties(rkavp_streaming PROPERTIES EXPORT_NAME Streaming)
