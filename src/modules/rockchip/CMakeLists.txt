add_library(rkavp_rockchip SHARED
  nodes.cpp
  plugin.cpp
  mpp/backend.cpp
  rga/backend.cpp
  rknn/backend.cpp
  v4l2/backend.cpp
)
add_library(RKAVP::Rockchip ALIAS rkavp_rockchip)
target_include_directories(rkavp_rockchip PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rkavp_rockchip
  PUBLIC RKAVP::Core
  PRIVATE RKAVP::MPP RKAVP::RGA RKAVP::RKNN
)
set_target_properties(rkavp_rockchip PROPERTIES
  OUTPUT_NAME rkavp_rockchip
  EXPORT_NAME Rockchip
)
