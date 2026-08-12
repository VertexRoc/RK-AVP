cmake_minimum_required(VERSION 3.18)
project(RKAVPConsumer LANGUAGES CXX)
find_package(RKAVP 0.1 REQUIRED)

add_library(rkavp_external_plugin SHARED plugin.cpp)
target_link_libraries(rkavp_external_plugin PRIVATE RKAVP::Core)
target_compile_features(rkavp_external_plugin PRIVATE cxx_std_17)

add_library(rkavp_external_plugin_v1 SHARED plugin_v1.cpp)
target_link_libraries(rkavp_external_plugin_v1 PRIVATE RKAVP::Core)
target_compile_features(rkavp_external_plugin_v1 PRIVATE cxx_std_17)

add_executable(rkavp_consumer main.cpp)
target_link_libraries(rkavp_consumer PRIVATE RKAVP::Core RKAVP::Testing)
target_compile_features(rkavp_consumer PRIVATE cxx_std_17)
target_compile_definitions(rkavp_consumer PRIVATE
  RKAVP_EXTERNAL_PLUGIN_PATH="$<TARGET_FILE:rkavp_external_plugin>"
  RKAVP_EXTERNAL_PLUGIN_V1_PATH="$<TARGET_FILE:rkavp_external_plugin_v1>"
)
add_dependencies(rkavp_consumer rkavp_external_plugin rkavp_external_plugin_v1)
