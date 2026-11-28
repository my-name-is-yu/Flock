find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(flock_t3 STATIC
  adapter.cpp
  discovery.cpp
  mcp.cpp
  oauth.cpp
  pinned_workspace.cpp
  recording.cpp
  session_read.cpp
)
target_link_libraries(flock_t3 PUBLIC flock_core PRIVATE CURL::libcurl OpenSSL::Crypto)
target_compile_options(flock_t3 PRIVATE ${flock_warnings})

if(BUILD_TESTING)
  flock_test_executable(flock_t3_recording_probe SOURCES recording_probe.cpp LIBRARIES flock_t3)
  add_test(NAME flock_t3_recording_contract COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/fixture.py"
    $<TARGET_FILE:flock_t3_recording_probe> recording-contract "${PROJECT_SOURCE_DIR}/tests/recordings/t3.jsonl")
  set_tests_properties(flock_t3_recording_contract PROPERTIES TIMEOUT 30)
  flock_test_executable(flock_t3_tests SOURCES tests.cpp session_search_tests.cpp LIBRARIES flock_t3)
  add_test(NAME flock_t3 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture.py" $<TARGET_FILE:flock_t3_tests>)
  flock_test_executable(flock_t3_multi_environment_tests SOURCES multi_environment_tests.cpp LIBRARIES flock_t3)
  add_test(NAME flock_t3_multi_environment COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture.py"
    $<TARGET_FILE:flock_t3_multi_environment_tests> multi-environment)
  flock_test_executable(flock_t3_pinned_workspace_tests SOURCES pinned_workspace_tests.cpp LIBRARIES flock_t3)
  add_test(NAME flock_t3_pinned_workspace COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture.py"
    $<TARGET_FILE:flock_t3_pinned_workspace_tests> pinned-workspace)
endif()
