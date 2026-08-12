add_library(rkavp_opencl SHARED
  backend.cpp
  nodes.cpp
  plugin.cpp
)
add_library(RKAVP::OpenCLBackend ALIAS rkavp_opencl)
target_include_directories(rkavp_opencl PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rkavp_opencl PUBLIC RKAVP::Core PRIVATE RKAVP::OpenCL)
set_target_properties(rkavp_opencl PROPERTIES EXPORT_NAME OpenCLBackend)
