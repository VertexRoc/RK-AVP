cmake_minimum_required(VERSION 3.18)
project(RKAVP_Prefetch NONE)

include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/../versions.cmake)

set(RKAVP_DEPS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../src" CACHE PATH
    "Directory containing pinned third-party source checkouts")
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "" FORCE)
set(FETCHCONTENT_QUIET OFF)

function(rkavp_use_prefetched_source content_name source_dir)
  if(EXISTS "${source_dir}/.git" OR EXISTS "${source_dir}/CMakeLists.txt")
    string(TOUPPER "${content_name}" content_upper)
    set("FETCHCONTENT_SOURCE_DIR_${content_upper}" "${source_dir}"
        CACHE PATH "Use the prefetched ${content_name} checkout" FORCE)
  endif()
endfunction()

rkavp_use_prefetched_source(rk_mpp "${RKAVP_DEPS_SOURCE_DIR}/rk_mpp")
rkavp_use_prefetched_source(rk_rga "${RKAVP_DEPS_SOURCE_DIR}/rk_rga")
rkavp_use_prefetched_source(zlmediakit "${RKAVP_DEPS_SOURCE_DIR}/zlmediakit")

FetchContent_Declare(rk_mpp
  GIT_REPOSITORY ${RKAVP_MPP_REPOSITORY}
  GIT_TAG ${RKAVP_MPP_TAG}
  GIT_SHALLOW FALSE
  SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/rk_mpp"
)
FetchContent_Populate(rk_mpp)

FetchContent_Declare(rk_rga
  GIT_REPOSITORY ${RKAVP_RGA_REPOSITORY}
  GIT_TAG ${RKAVP_RGA_TAG}
  GIT_SHALLOW FALSE
  SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/rk_rga"
)
FetchContent_Populate(rk_rga)

execute_process(
  COMMAND ${CMAKE_COMMAND}
    -DREPOSITORY=${RKAVP_RKNN_REPOSITORY}
    -DREVISION=${RKAVP_RKNN_TAG}
    -DSOURCE_DIR=${RKAVP_DEPS_SOURCE_DIR}/rknn-runtime
    -DSPARSE_PATH=rknpu2/runtime/Linux/librknn_api
    -P ${CMAKE_CURRENT_LIST_DIR}/../sparse_clone.cmake
  COMMAND_ERROR_IS_FATAL ANY
)

FetchContent_Declare(zlmediakit
  GIT_REPOSITORY ${RKAVP_ZLMEDIAKIT_REPOSITORY}
  GIT_TAG ${RKAVP_ZLMEDIAKIT_TAG}
  GIT_SUBMODULES_RECURSE TRUE
  SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/zlmediakit"
)
FetchContent_Populate(zlmediakit)
