include(FetchContent)
include(ExternalProject)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  message(FATAL_ERROR "Rockchip backends require an aarch64 target toolchain")
endif()

set(BUILD_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(rk_mpp
  GIT_REPOSITORY ${RKAVP_MPP_REPOSITORY}
  GIT_TAG ${RKAVP_MPP_TAG}
  SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/rk_mpp"
  BINARY_DIR "${CMAKE_BINARY_DIR}/3rdparty/rk_mpp-build"
  SUBBUILD_DIR "${CMAKE_BINARY_DIR}/3rdparty/rk_mpp-subbuild"
)
set(rkavp_saved_c_extensions "${CMAKE_C_EXTENSIONS}")
set(rkavp_saved_cxx_extensions "${CMAKE_CXX_EXTENSIONS}")
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_EXTENSIONS ON)
FetchContent_MakeAvailable(rk_mpp)
set(CMAKE_C_EXTENSIONS "${rkavp_saved_c_extensions}")
set(CMAKE_CXX_EXTENSIONS "${rkavp_saved_cxx_extensions}")
if(TARGET rockchip_vpu)
  target_link_directories(rockchip_vpu PRIVATE "${rk_mpp_BINARY_DIR}/mpp")
endif()
add_library(RKAVP_MPP INTERFACE)
target_link_libraries(RKAVP_MPP INTERFACE rockchip_mpp)
target_include_directories(RKAVP_MPP INTERFACE
  "${rk_mpp_SOURCE_DIR}/inc"
  "${rk_mpp_SOURCE_DIR}/osal/inc"
  "${rk_mpp_SOURCE_DIR}/mpp/base/inc"
)
add_library(RKAVP::MPP ALIAS RKAVP_MPP)

FetchContent_Declare(rk_rga
  GIT_REPOSITORY ${RKAVP_RGA_REPOSITORY}
  GIT_TAG ${RKAVP_RGA_TAG}
  SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/rk_rga"
)
FetchContent_GetProperties(rk_rga)
if(NOT rk_rga_POPULATED)
  FetchContent_Populate(rk_rga)
endif()
add_library(RKAVP_RGA_Binary SHARED IMPORTED GLOBAL)
set_target_properties(RKAVP_RGA_Binary PROPERTIES
  IMPORTED_LOCATION "${rk_rga_SOURCE_DIR}/libs/Linux/gcc-aarch64/librga.so"
  INTERFACE_INCLUDE_DIRECTORIES "${rk_rga_SOURCE_DIR}/include"
)
add_library(RKAVP_RGA INTERFACE)
target_link_libraries(RKAVP_RGA INTERFACE RKAVP_RGA_Binary)
add_library(RKAVP::RGA ALIAS RKAVP_RGA)

set(rknn_toolkit2_SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/rknn-runtime")
ExternalProject_Add(RKAVP_RKNN_Download
  DOWNLOAD_COMMAND ${CMAKE_COMMAND}
    -DREPOSITORY=${RKAVP_RKNN_REPOSITORY}
    -DREVISION=${RKAVP_RKNN_TAG}
    -DSOURCE_DIR=${rknn_toolkit2_SOURCE_DIR}
    -DSPARSE_PATH=rknpu2/runtime/Linux/librknn_api
    -P ${CMAKE_CURRENT_LIST_DIR}/sparse_clone.cmake
  SOURCE_DIR "${rknn_toolkit2_SOURCE_DIR}"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  TEST_COMMAND ""
)
add_library(RKAVP_RKNN SHARED IMPORTED GLOBAL)
set_target_properties(RKAVP_RKNN PROPERTIES
  IMPORTED_LOCATION "${rknn_toolkit2_SOURCE_DIR}/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so"
  INTERFACE_INCLUDE_DIRECTORIES "${rknn_toolkit2_SOURCE_DIR}/rknpu2/runtime/Linux/librknn_api/include"
)
add_dependencies(RKAVP_RKNN RKAVP_RKNN_Download)
add_library(RKAVP::RKNN ALIAS RKAVP_RKNN)
