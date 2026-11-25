find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(flock_t3 mcp.cpp oauth.cpp discovery.cpp adapter.cpp session_read.cpp pinned_workspace.cpp)
target_link_libraries(flock_t3 PUBLIC flock_core PRIVATE CURL::libcurl OpenSSL::Crypto)
target_compile_options(flock_t3 PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_executable(flock_t3_tests tests.cpp session_search_tests.cpp)
  target_link_libraries(flock_t3_tests PRIVATE flock_t3)
  add_test(NAME flock_t3 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture.py" $<TARGET_FILE:flock_t3_tests>)
  add_executable(flock_t3_peer_recovery_tests peer_recovery.cpp)
  target_link_libraries(flock_t3_peer_recovery_tests PRIVATE flock_t3)
  add_test(NAME flock_t3_peer_recovery COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture.py"
    $<TARGET_FILE:flock_t3_peer_recovery_tests> peer-recovery)
  add_executable(flock_t3_multi_environment_tests multi_environment_tests.cpp)
  target_link_libraries(flock_t3_multi_environment_tests PRIVATE flock_t3)
  add_test(NAME flock_t3_multi_environment COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture.py"
    $<TARGET_FILE:flock_t3_multi_environment_tests> multi-environment)
  add_executable(flock_t3_pinned_workspace_tests pinned_workspace_tests.cpp)
  target_link_libraries(flock_t3_pinned_workspace_tests PRIVATE flock_t3)
  add_test(NAME flock_t3_pinned_workspace COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture.py"
    $<TARGET_FILE:flock_t3_pinned_workspace_tests> pinned-workspace)
endif()
