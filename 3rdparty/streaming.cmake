include(FetchContent)
FetchContent_Declare(zlmediakit
  GIT_REPOSITORY ${RKAVP_ZLMEDIAKIT_REPOSITORY}
  GIT_TAG ${RKAVP_ZLMEDIAKIT_TAG}
  GIT_SUBMODULES_RECURSE TRUE
  SOURCE_DIR "${RKAVP_DEPS_SOURCE_DIR}/zlmediakit"
)
set(ENABLE_WEBRTC OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SERVER OFF CACHE BOOL "" FORCE)
set(ENABLE_SRT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLS OFF CACHE BOOL "" FORCE)
set(ENABLE_MP4 OFF CACHE BOOL "" FORCE)
set(ENABLE_RTPPROXY ON CACHE BOOL "" FORCE)
set(ENABLE_CXX_API OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJCOPY OFF CACHE BOOL "" FORCE)

# Keep the upstream project outside RK-AVP's public SDK install surface. Linked
# targets are still built, but ZLMediaKit's own headers and CMake install rules
# are excluded; RK-AVP installs only the runtime C API shared object below.
FetchContent_GetProperties(zlmediakit)
if(NOT zlmediakit_POPULATED)
  FetchContent_Populate(zlmediakit)
  add_subdirectory("${zlmediakit_SOURCE_DIR}" "${zlmediakit_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()

# ZLMediaKit's AV1 adapter is always part of ext-codec, while the small AV1
# configuration-record implementation is normally pulled in by ENABLE_MP4.
# Keep MP4 disabled for the framework runtime and attach only that dependency.
if(TARGET ext-codec AND NOT TARGET flv)
  set(_rkavp_zlm_flv_root
      "${zlmediakit_SOURCE_DIR}/3rdpart/media-server/libflv")
  target_sources(ext-codec PRIVATE
    "${_rkavp_zlm_flv_root}/source/aom-av1.c"
    "${_rkavp_zlm_flv_root}/source/opus-head.c"
    "${_rkavp_zlm_flv_root}/source/riff-acm.c"
    "${_rkavp_zlm_flv_root}/source/webm-vpx.c"
  )
  target_include_directories(ext-codec PRIVATE
    "${_rkavp_zlm_flv_root}/include"
  )
endif()

# The recorder C API is still globbed into mk_api when MP4 is disabled, even
# though its implementation requires the recording/server muxer definition.
# RK-AVP exposes streaming sessions, not ZLMediaKit's recorder API.
if(TARGET mk_api AND NOT ENABLE_MP4)
  get_target_property(_rkavp_zlm_api_sources mk_api SOURCES)
  list(FILTER _rkavp_zlm_api_sources EXCLUDE REGEX "[/\\\\]mk_recorder\\.cpp$")
  set_property(TARGET mk_api PROPERTY SOURCES "${_rkavp_zlm_api_sources}")
endif()

add_library(RKAVP_ZLMediaKit INTERFACE)
if(TARGET mk_api)
  target_link_libraries(RKAVP_ZLMediaKit INTERFACE mk_api)
  install(TARGETS mk_api
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
  )
else()
  message(FATAL_ERROR "ZLMediaKit did not export mk_api")
endif()
add_library(RKAVP::ZLMediaKit ALIAS RKAVP_ZLMediaKit)
