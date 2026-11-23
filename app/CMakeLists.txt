find_package(tomlplusplus 3.4 CONFIG QUIET)
if(NOT TARGET tomlplusplus::tomlplusplus)
  FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tomlplusplus)
endif()

configure_file(chatgpt.mjs chatgpt.mjs COPYONLY)
add_library(flock_app_support bots.cpp model.cpp bot_tools.cpp git_github.cpp t3_setup.cpp)
target_link_libraries(flock_app_support PUBLIC flock_core PRIVATE CURL::libcurl tomlplusplus::tomlplusplus)
target_compile_definitions(flock_app_support PRIVATE FLOCK_CHATGPT_BRIDGE="${CMAKE_CURRENT_BINARY_DIR}/chatgpt.mjs")
target_compile_options(flock_app_support PRIVATE -Wall -Wextra -Wpedantic)

add_executable(flock main.cpp demo.cpp settings_file.cpp settings_system.cpp settings_access.cpp settings_connections.cpp)
target_link_libraries(flock PRIVATE flock_app_support CURL::libcurl OpenSSL::Crypto)
target_compile_options(flock PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_executable(flock_app_settings_access tests/settings_access.cpp settings_access.cpp)
  target_link_libraries(flock_app_settings_access PRIVATE flock_core OpenSSL::Crypto)
  add_test(NAME flock_app_settings_access COMMAND flock_app_settings_access)
  add_executable(flock_app_tests tests.cpp session_tests.cpp tests/schedules.cpp demo.cpp)
  target_link_libraries(flock_app_tests PRIVATE flock_app_support)
  add_test(NAME flock_app COMMAND flock_app_tests)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(flock_app_session_settings tests/session_settings.cpp)
  target_link_libraries(flock_app_session_settings PRIVATE flock_app_support flock_t3)
  add_test(NAME flock_app_session_settings COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/t3/fixture.py" $<TARGET_FILE:flock_app_session_settings>)
  add_test(NAME flock_cli_invalid_port
    COMMAND "${CMAKE_COMMAND}" "-DFLOCK=$<TARGET_FILE:flock>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_port.cmake")
  find_program(FLOCK_NODE_EXECUTABLE node)
  if(FLOCK_NODE_EXECUTABLE)
    add_test(NAME flock_app_headless_settings
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless-settings.mjs" "$<TARGET_FILE:flock>")
    set_tests_properties(flock_app_headless_settings PROPERTIES TIMEOUT 20)
    add_test(NAME flock_app_installation_system
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/installation-system.mjs" "$<TARGET_FILE:flock>")
    set_tests_properties(flock_app_installation_system PROPERTIES TIMEOUT 20)
    add_test(NAME flock_app_login
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/login.mjs" "$<TARGET_FILE:flock>")
    set_tests_properties(flock_app_login PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 25)
  endif()
endif()
