include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/versions.cmake)

set(RKAVP_DEPS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/src" CACHE PATH
    "Directory containing pinned third-party source checkouts")
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
    "Per-build dependency work directory" FORCE)
set(FETCHCONTENT_QUIET OFF)

function(rkavp_use_prefetched_source content_name source_dir)
  if(EXISTS "${source_dir}/.git" OR EXISTS "${source_dir}/CMakeLists.txt")
    string(TOUPPER "${content_name}" content_upper)
    set("FETCHCONTENT_SOURCE_DIR_${content_upper}" "${source_dir}"
        CACHE PATH "Use the prefetched ${content_name} checkout" FORCE)
  endif()
endfunction()

rkavp_use_prefetched_source(yaml-cpp "${RKAVP_DEPS_SOURCE_DIR}/yaml-cpp")
rkavp_use_prefetched_source(googletest "${RKAVP_DEPS_SOURCE_DIR}/googletest")
rkavp_use_prefetched_source(pybind11 "${RKAVP_DEPS_SOURCE_DIR}/pybind11")
rkavp_use_prefetched_source(rk_mpp "${RKAVP_DEPS_SOURCE_DIR}/rk_mpp")
rkavp_use_prefetched_source(rk_rga "${RKAVP_DEPS_SOURCE_DIR}/rk_rga")
rkavp_use_prefetched_source(zlmediakit "${RKAVP_DEPS_SOURCE_DIR}/zlmediakit")
find_package(Threads REQUIRED)
add_library(RKAVP_Threads INTERFACE)
target_link_libraries(RKAVP_Threads INTERFACE Threads::Threads)
add_library(RKAVP::Threads ALIAS RKAVP_Threads)

if(NOT RKAVP_FORCE_FETCH_DEPS)
  find_package(yaml-cpp QUIET)
endif()
if(NOT TARGET yaml-cpp::yaml-cpp AND NOT TARGET yaml-cpp)
  if(NOT RKAVP_FETCH_DEPS)
    message(FATAL_ERROR "yaml-cpp was not found and RKAVP_FETCH_DEPS=OFF")
  endif()
  FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG ${RKAVP_YAML_CPP_TAG}
    GIT_SHALLOW TRUE
    SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/yaml-cpp"
  )
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(yaml-cpp)
endif()
add_library(RKAVP_Yaml INTERFACE)
if(TARGET yaml-cpp::yaml-cpp)
  set(rkavp_yaml_build_target yaml-cpp::yaml-cpp)
else()
  set(rkavp_yaml_build_target yaml-cpp)
endif()
target_link_libraries(RKAVP_Yaml INTERFACE
  $<BUILD_INTERFACE:${rkavp_yaml_build_target}>
  $<INSTALL_INTERFACE:yaml-cpp::yaml-cpp>
)
add_library(RKAVP::Yaml ALIAS RKAVP_Yaml)

if(RKAVP_BUILD_TESTS)
  if(NOT RKAVP_FORCE_FETCH_DEPS)
    find_package(GTest QUIET)
  endif()
  if(NOT TARGET GTest::gtest)
    if(NOT RKAVP_FETCH_DEPS)
      message(FATAL_ERROR "GoogleTest was not found and RKAVP_FETCH_DEPS=OFF")
    endif()
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG ${RKAVP_GOOGLETEST_TAG}
      GIT_SHALLOW TRUE
      SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/googletest"
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_library(RKAVP_GTestMain INTERFACE)
  target_link_libraries(RKAVP_GTestMain INTERFACE GTest::gtest_main)
  add_library(RKAVP::GTestMain ALIAS RKAVP_GTestMain)
  if(TARGET GTest::gmock)
    add_library(RKAVP_GMock INTERFACE)
    target_link_libraries(RKAVP_GMock INTERFACE GTest::gmock)
    add_library(RKAVP::GMock ALIAS RKAVP_GMock)
  endif()
endif()

if(RKAVP_ENABLE_PYTHON)
  if(NOT RKAVP_FORCE_FETCH_DEPS)
    find_package(pybind11 QUIET)
  endif()
  if(NOT TARGET pybind11::module)
    FetchContent_Declare(pybind11
      GIT_REPOSITORY https://github.com/pybind/pybind11.git
      GIT_TAG ${RKAVP_PYBIND11_TAG}
      GIT_SHALLOW TRUE
      SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/pybind11"
    )
    FetchContent_MakeAvailable(pybind11)
  endif()
endif()

if(RKAVP_ENABLE_AUDIO)
  find_package(ALSA REQUIRED)
  add_library(RKAVP_ALSA INTERFACE)
  target_link_libraries(RKAVP_ALSA INTERFACE ALSA::ALSA)
  add_library(RKAVP::ALSA ALIAS RKAVP_ALSA)
endif()

if(RKAVP_ENABLE_ROCKCHIP)
  include(${CMAKE_CURRENT_LIST_DIR}/rockchip.cmake)
endif()

if(RKAVP_ENABLE_OPENCL)
  find_package(OpenCL REQUIRED)
  add_library(RKAVP_OpenCL INTERFACE)
  target_link_libraries(RKAVP_OpenCL INTERFACE OpenCL::OpenCL)
  add_library(RKAVP::OpenCL ALIAS RKAVP_OpenCL)
endif()

if(RKAVP_ENABLE_STREAMING)
  include(${CMAKE_CURRENT_LIST_DIR}/streaming.cmake)
endif()
