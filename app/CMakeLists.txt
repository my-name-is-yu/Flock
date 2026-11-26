find_package(Threads REQUIRED)
find_package(tomlplusplus 3.4 CONFIG QUIET)
if(NOT TARGET tomlplusplus::tomlplusplus)
  FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tomlplusplus)
endif()

add_library(flock_app_support STATIC bots.cpp model.cpp demo.cpp chatgpt.cpp chatgpt_browser.cpp chatgpt_http.cpp chatgpt_oauth.cpp chatgpt_recording.cpp
                             bot_tools.cpp git_github.cpp t3_setup.cpp settings_file.cpp)
target_include_directories(flock_app_support PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(flock_app_support PUBLIC flock_core
                      PRIVATE CURL::libcurl OpenSSL::Crypto Threads::Threads tomlplusplus::tomlplusplus)
target_compile_options(flock_app_support PRIVATE -Wall -Wextra -Wpedantic)

add_executable(flock main.cpp chatgpt_web.cpp settings_system.cpp settings_access.cpp settings_connections.cpp
                     t3_connection_setup.cpp)
target_link_libraries(flock PRIVATE flock_app_support CURL::libcurl OpenSSL::Crypto)
target_compile_options(flock PRIVATE -Wall -Wextra -Wpedantic)
set_property(TARGET flock PROPERTY INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS flock)

if(BUILD_TESTING)
  add_executable(flock_app_settings_access tests/settings_access.cpp settings_access.cpp)
  target_link_libraries(flock_app_settings_access PRIVATE flock_core OpenSSL::Crypto)
  add_test(NAME flock_app_settings_access COMMAND flock_app_settings_access)
  add_executable(flock_app_tests tests.cpp session_tests.cpp tests/conversation_routing.cpp tests/schedules.cpp)
  target_link_libraries(flock_app_tests PRIVATE flock_app_support)
  add_test(NAME flock_app COMMAND flock_app_tests)
  add_executable(flock_app_chatgpt_transport tests/chatgpt_transport.cpp)
  target_link_libraries(flock_app_chatgpt_transport PRIVATE flock_app_support)
  target_compile_options(flock_app_chatgpt_transport PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_app_chatgpt_transport COMMAND flock_app_chatgpt_transport)
  add_test(NAME flock_app_chatgpt_recordings COMMAND flock_app_chatgpt_transport --recordings
    "${PROJECT_SOURCE_DIR}/tests/recordings/chatgpt.jsonl")
  add_executable(flock_app_chatgpt_oauth tests/chatgpt_oauth.cpp chatgpt_web.cpp settings_access.cpp)
  target_link_libraries(flock_app_chatgpt_oauth PRIVATE flock_app_support flock_web CURL::libcurl OpenSSL::Crypto)
  target_compile_options(flock_app_chatgpt_oauth PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_app_chatgpt_oauth COMMAND flock_app_chatgpt_oauth)
  add_executable(flock_app_chatgpt_browser tests/chatgpt_browser.cpp)
  target_link_libraries(flock_app_chatgpt_browser PRIVATE flock_app_support)
  target_compile_options(flock_app_chatgpt_browser PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_app_chatgpt_browser COMMAND flock_app_chatgpt_browser)
  set_tests_properties(flock_app_chatgpt_oauth flock_app_chatgpt_browser PROPERTIES TIMEOUT 15)
  if(FLOCK_TEST_OPENSSL)
    add_test(NAME flock_app_chatgpt_tls
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_tls.py"
              "$<TARGET_FILE:flock_app_chatgpt_transport>" "${FLOCK_TEST_OPENSSL}")
    set_tests_properties(flock_app_chatgpt_tls PROPERTIES TIMEOUT 15)
  endif()
  add_executable(flock_app_session_settings tests/session_settings.cpp)
  target_link_libraries(flock_app_session_settings PRIVATE flock_app_support flock_t3)
  add_test(NAME flock_app_session_settings COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/t3/fixture.py" $<TARGET_FILE:flock_app_session_settings>)
  add_test(NAME flock_cli_invalid_port
    COMMAND "${CMAKE_COMMAND}" "-DFLOCK=$<TARGET_FILE:flock>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_port.cmake")
  if(FLOCK_NODE_EXECUTABLE)
    add_test(NAME flock_app_bot_deletion
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot-deletion.mjs"
              "$<TARGET_FILE:flock>" "${Python3_EXECUTABLE}")
    set_tests_properties(flock_app_bot_deletion PROPERTIES TIMEOUT 25)
    add_test(NAME flock_cli_bot_pause
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot-pause.mjs" "$<TARGET_FILE:flock>")
    set_tests_properties(flock_cli_bot_pause PROPERTIES TIMEOUT 20)
    add_test(NAME flock_app_headless_settings
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/headless-settings.mjs" "$<TARGET_FILE:flock>")
    set_tests_properties(flock_app_headless_settings PROPERTIES TIMEOUT 20)
    add_test(NAME flock_app_installation_system
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/installation-system.mjs" "$<TARGET_FILE:flock>")
    set_tests_properties(flock_app_installation_system PROPERTIES TIMEOUT 20)
  endif()
endif()
