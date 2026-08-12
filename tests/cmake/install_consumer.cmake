set(prefix "${RKAVP_CONSUMER_BINARY_DIR}/prefix")
file(REMOVE_RECURSE "${prefix}" "${RKAVP_CONSUMER_BINARY_DIR}/build")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${RKAVP_BUILD_DIR}" --prefix "${prefix}"
    --component Runtime
  RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "RK-AVP Runtime install failed: ${install_result}")
endif()
if(EXISTS "${prefix}/include" OR EXISTS "${prefix}/lib/librkavp_testing.a" OR
   EXISTS "${prefix}/share/doc" OR EXISTS "${prefix}/share/rkavp/graphs" OR
   EXISTS "${prefix}/share/rkavp/examples")
  message(FATAL_ERROR "RK-AVP Runtime component contains development, testing, documentation, or example artifacts")
endif()
foreach(component Development Testing)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${RKAVP_BUILD_DIR}" --prefix "${prefix}"
      --component "${component}"
    RESULT_VARIABLE install_result
  )
  if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "RK-AVP ${component} install failed: ${install_result}")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -S "${RKAVP_SOURCE_DIR}/tests/install-consumer"
    -B "${RKAVP_CONSUMER_BINARY_DIR}/build"
    "-DCMAKE_PREFIX_PATH=${prefix}"
  RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "installed RK-AVP consumer configure failed: ${configure_result}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${RKAVP_CONSUMER_BINARY_DIR}/build"
  RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "installed RK-AVP consumer build failed: ${build_result}")
endif()

execute_process(
  COMMAND "${RKAVP_CONSUMER_BINARY_DIR}/build/rkavp_consumer"
  RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "installed RK-AVP consumer run failed: ${run_result}")
endif()
