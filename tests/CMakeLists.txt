include(GoogleTest)
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

add_executable(rkavp_unit_tests
  ${PROJECT_SOURCE_DIR}/src/modules/audio/nodes.cpp
  ${PROJECT_SOURCE_DIR}/src/modules/opencl/nodes.cpp
  ${PROJECT_SOURCE_DIR}/src/modules/rockchip/nodes.cpp
  ${PROJECT_SOURCE_DIR}/src/modules/streaming/nodes.cpp
  unit/test_backends.cpp
  unit/test_core.cpp
  unit/test_graph.cpp
  unit/test_logging.cpp
  unit/test_node_test_runner.cpp
  unit/test_queue_scheduler.cpp
)
target_link_libraries(rkavp_unit_tests PRIVATE RKAVP::Core RKAVP::Testing RKAVP::GTestMain)
add_library(rkavp_test_plugin SHARED support/test_plugin_module.cpp)
target_link_libraries(rkavp_test_plugin PRIVATE RKAVP::Core)
add_library(rkavp_test_plugin_v1 SHARED support/test_plugin_v1_module.cpp)
target_link_libraries(rkavp_test_plugin_v1 PRIVATE RKAVP::Core)
target_compile_definitions(rkavp_unit_tests PRIVATE
  RKAVP_TEST_PLUGIN_PATH="$<TARGET_FILE:rkavp_test_plugin>"
  RKAVP_TEST_PLUGIN_V1_PATH="$<TARGET_FILE:rkavp_test_plugin_v1>"
)
add_dependencies(rkavp_unit_tests rkavp_test_plugin rkavp_test_plugin_v1)
target_compile_options(rkavp_unit_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(rkavp_unit_tests PROPERTIES LABELS "unit")

add_executable(rkavp_integration_tests
  integration/test_runner.cpp
  integration/test_runtime_features.cpp
)
target_link_libraries(rkavp_integration_tests PRIVATE RKAVP::Core RKAVP::GTestMain)
target_compile_options(rkavp_integration_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(rkavp_integration_tests PROPERTIES LABELS "integration")

add_test(NAME rkavp_installed_package
  COMMAND ${CMAKE_COMMAND}
    -DRKAVP_BUILD_DIR=${CMAKE_BINARY_DIR}
    -DRKAVP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DRKAVP_CONSUMER_BINARY_DIR=${CMAKE_BINARY_DIR}/installed-consumer
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_consumer.cmake
)
set_tests_properties(rkavp_installed_package PROPERTIES LABELS "integration")

add_test(NAME rkavp_architecture_boundary
  COMMAND ${CMAKE_COMMAND}
    -DRKAVP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DRKAVP_BUILD_DIR=${CMAKE_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_core_boundary.cmake
)
set_tests_properties(rkavp_architecture_boundary PROPERTIES LABELS "unit")

add_test(NAME rkavp_ctk_doctor COMMAND rkavp-ctk doctor)
set_tests_properties(rkavp_ctk_doctor PROPERTIES LABELS "integration")
add_test(NAME rkavp_ctk_cdi_generate
  COMMAND rkavp-ctk cdi generate --output ${CMAKE_BINARY_DIR}/test-cdi)
set_tests_properties(rkavp_ctk_cdi_generate PROPERTIES LABELS "integration")

if(RKAVP_BUILD_HARDWARE_TESTS)
  if(NOT RKAVP_ENABLE_ROCKCHIP)
    message(FATAL_ERROR "RKAVP_BUILD_HARDWARE_TESTS requires RKAVP_ENABLE_ROCKCHIP=ON")
  endif()

  add_executable(rkavp_hardware_tests hardware/test_hardware.cpp)
  target_link_libraries(rkavp_hardware_tests PRIVATE RKAVP::Core RKAVP::GTestMain)
  target_compile_definitions(rkavp_hardware_tests PRIVATE
    RKAVP_ROCKCHIP_PLUGIN_PATH="$<TARGET_FILE:rkavp_rockchip>"
  )
  add_dependencies(rkavp_hardware_tests rkavp_rockchip)
  if(RKAVP_ENABLE_AUDIO)
    target_compile_definitions(rkavp_hardware_tests PRIVATE
      RKAVP_AUDIO_PLUGIN_PATH="$<TARGET_FILE:rkavp_audio>"
    )
    add_dependencies(rkavp_hardware_tests rkavp_audio)
  endif()
  if(RKAVP_ENABLE_STREAMING)
    target_compile_definitions(rkavp_hardware_tests PRIVATE
      RKAVP_STREAMING_PLUGIN_PATH="$<TARGET_FILE:rkavp_streaming>"
    )
    add_dependencies(rkavp_hardware_tests rkavp_streaming)
  endif()
  gtest_discover_tests(rkavp_hardware_tests PROPERTIES LABELS "hardware")

  add_executable(rkavp_endurance_tests performance/test_endurance.cpp)
  target_link_libraries(rkavp_endurance_tests PRIVATE RKAVP::Core RKAVP::GTestMain)
  gtest_discover_tests(rkavp_endurance_tests PROPERTIES LABELS "performance")
endif()

if(RKAVP_ENABLE_PYTHON)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME rkavp_python_tests
    COMMAND ${CMAKE_COMMAND} -E env
      "PYTHONPATH=${CMAKE_BINARY_DIR}/python"
      ${Python3_EXECUTABLE} -m pytest -q "${CMAKE_CURRENT_SOURCE_DIR}/python"
  )
  set_tests_properties(rkavp_python_tests PROPERTIES LABELS "python")
endif()
