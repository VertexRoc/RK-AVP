cmake_minimum_required(VERSION 3.18)

project(RK_AVP VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN")

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)
include(cmake/RKAVPOptions.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/CMakeLists.txt)

add_custom_target(rkavp-prefetch
  COMMAND ${CMAKE_COMMAND}
    -S "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/prefetch"
    -B "${CMAKE_BINARY_DIR}/deps-prefetch"
  COMMENT "Prefetching pinned third-party sources into 3rdparty/src"
  USES_TERMINAL
)

add_library(rkavp_core SHARED
  src/core/buffer.cpp
  src/core/builtin_nodes.cpp
  src/core/config_value.cpp
  src/core/graph.cpp
  src/core/graph_config.cpp
  src/core/graph_runner.cpp
  src/core/logging.cpp
  src/core/metrics.cpp
  src/core/node.cpp
  src/core/node_registry.cpp
  src/core/platform.cpp
  src/core/plugin.cpp
  src/core/scheduler.cpp
  src/core/services.cpp
  src/core/streaming_primitives.cpp
  src/core/trace.cpp
)
add_library(RKAVP::Core ALIAS rkavp_core)
set_target_properties(rkavp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(rkavp_core PROPERTIES EXPORT_NAME Core)

target_include_directories(rkavp_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rkavp_core
  PUBLIC RKAVP::Threads ${CMAKE_DL_LIBS}
  PRIVATE RKAVP::Yaml
)
target_compile_options(rkavp_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(rkavp_core PUBLIC
  RKAVP_VERSION="${PROJECT_VERSION}"
  RKAVP_TARGET_SOC="${RKAVP_TARGET_SOC}"
)

add_library(rkavp_testing STATIC
  src/testing/node_test_runner.cpp
)
add_library(RKAVP::Testing ALIAS rkavp_testing)
set_target_properties(rkavp_testing PROPERTIES
  EXPORT_NAME Testing
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(rkavp_testing
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rkavp_testing PUBLIC RKAVP::Core)
target_compile_options(rkavp_testing PRIVATE -Wall -Wextra -Wpedantic)
if(RKAVP_ENABLE_SANITIZERS)
  target_compile_options(rkavp_testing PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(rkavp_testing PUBLIC -fsanitize=address,undefined)
elseif(RKAVP_ENABLE_THREAD_SANITIZER)
  target_compile_options(rkavp_testing PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(rkavp_testing PUBLIC -fsanitize=thread)
elseif(RKAVP_ENABLE_COVERAGE)
  # Testing is static, so installed consumers must also link the gcov runtime.
  target_link_options(rkavp_testing PUBLIC --coverage)
endif()

if(RKAVP_ENABLE_ROCKCHIP)
  add_subdirectory(src/modules/rockchip)
endif()

if(RKAVP_ENABLE_AUDIO)
  add_subdirectory(src/modules/audio)
endif()

if(RKAVP_ENABLE_STREAMING)
  add_subdirectory(src/modules/streaming)
endif()

if(RKAVP_ENABLE_OPENCL)
  add_subdirectory(src/modules/opencl)
endif()

add_executable(rkavp-run tools/rkavp-run/main.cpp)
target_link_libraries(rkavp-run PRIVATE RKAVP::Core)
set_target_properties(rkavp-run PROPERTIES
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
)

add_executable(rkavp-ctk tools/rkavp-ctk/main.cpp)
target_link_libraries(rkavp-ctk PRIVATE RKAVP::Core)
set_target_properties(rkavp-ctk PROPERTIES
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
)

if(RKAVP_ENABLE_PYTHON)
  pybind11_add_module(_rkavp python/bindings.cpp)
  target_link_libraries(_rkavp PRIVATE RKAVP::Core)
  set_target_properties(_rkavp PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python/rkavp")
  add_custom_command(TARGET _rkavp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/python/rkavp/__init__.py"
      "${CMAKE_BINARY_DIR}/python/rkavp/__init__.py"
  )
endif()

if(RKAVP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RKAVP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

set(rkavp_runtime_targets rkavp_core)
set_target_properties(RKAVP_Threads PROPERTIES EXPORT_NAME Threads)

if(RKAVP_ENABLE_ROCKCHIP)
  list(APPEND rkavp_runtime_targets rkavp_rockchip)
endif()
if(RKAVP_ENABLE_AUDIO)
  list(APPEND rkavp_runtime_targets rkavp_audio)
endif()
if(RKAVP_ENABLE_STREAMING)
  list(APPEND rkavp_runtime_targets rkavp_streaming)
endif()
if(RKAVP_ENABLE_OPENCL)
  list(APPEND rkavp_runtime_targets rkavp_opencl)
endif()

install(TARGETS ${rkavp_runtime_targets} rkavp-run rkavp-ctk
  EXPORT RKAVPTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
)
install(TARGETS RKAVP_Threads
  EXPORT RKAVPTargets
  COMPONENT Development
)
install(TARGETS rkavp_testing
  EXPORT RKAVPTestingTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Testing
)
install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT Development
  PATTERN "testing" EXCLUDE
)
install(DIRECTORY include/rkavp/testing/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rkavp/testing
  COMPONENT Testing
)
install(DIRECTORY graphs/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/rkavp/graphs
  COMPONENT Examples
)
install(DIRECTORY examples/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/rkavp/examples
  COMPONENT Examples
)
install(DIRECTORY docs/
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
  COMPONENT Documentation
)
install(DIRECTORY deploy/systemd/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/rkavp/systemd
  COMPONENT Runtime
)
install(FILES deploy/compatibility.yaml
  DESTINATION ${CMAKE_INSTALL_DATADIR}/rkavp
  COMPONENT Runtime
)
install(FILES LICENSE THIRD_PARTY_NOTICES.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/RK-AVP
  COMPONENT Runtime
)
if(RKAVP_ENABLE_ROCKCHIP AND EXISTS "${rk_rga_SOURCE_DIR}/COPYING")
  install(FILES "${rk_rga_SOURCE_DIR}/COPYING"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/RK-AVP
    RENAME librga-COPYING
    COMPONENT Runtime
  )
endif()
if(RKAVP_ENABLE_STREAMING AND EXISTS "${zlmediakit_SOURCE_DIR}/LICENSE")
  install(FILES "${zlmediakit_SOURCE_DIR}/LICENSE"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/RK-AVP
    RENAME ZLMediaKit-LICENSE
    COMPONENT Runtime
  )
endif()
install(EXPORT RKAVPTargets
  FILE RKAVPTargets.cmake
  NAMESPACE RKAVP::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RKAVP
  COMPONENT Development
)
install(EXPORT RKAVPTestingTargets
  FILE RKAVPTestingTargets.cmake
  NAMESPACE RKAVP::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RKAVP
  COMPONENT Testing
)
configure_package_config_file(
  cmake/RKAVPConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/RKAVPConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RKAVP
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/RKAVPConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/RKAVPConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/RKAVPConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RKAVP
  COMPONENT Development
)
