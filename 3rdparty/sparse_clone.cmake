if(NOT DEFINED REPOSITORY OR NOT DEFINED REVISION OR NOT DEFINED SOURCE_DIR OR NOT DEFINED SPARSE_PATH)
  message(FATAL_ERROR "REPOSITORY, REVISION, SOURCE_DIR, and SPARSE_PATH are required")
endif()

find_package(Git REQUIRED)

if(EXISTS "${SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE current_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(current_revision STREQUAL REVISION AND EXISTS "${SOURCE_DIR}/${SPARSE_PATH}")
    message(STATUS "Sparse source is already at ${REVISION}: ${SOURCE_DIR}")
    return()
  endif()
endif()

file(REMOVE_RECURSE "${SOURCE_DIR}")
execute_process(
  COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none --no-checkout "${REPOSITORY}" "${SOURCE_DIR}"
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND ${GIT_EXECUTABLE} -C "${SOURCE_DIR}" sparse-checkout init --no-cone
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND ${GIT_EXECUTABLE} -C "${SOURCE_DIR}" sparse-checkout set "${SPARSE_PATH}"
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND ${GIT_EXECUTABLE} -C "${SOURCE_DIR}" checkout "${REVISION}"
  COMMAND_ERROR_IS_FATAL ANY
)
