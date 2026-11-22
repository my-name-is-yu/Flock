string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(home "${CMAKE_CURRENT_BINARY_DIR}/flock-invalid-port-${suffix}")
execute_process(COMMAND "${FLOCK}" --home "${home}" run --port 65536
  RESULT_VARIABLE status TIMEOUT 5)

if(EXISTS "${home}")
  file(REMOVE_RECURSE "${home}")
  message(FATAL_ERROR "Rejected CLI arguments created a Flock home")
endif()
if(NOT status MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "Expected a nonzero CLI exit code, received ${status}")
endif()
