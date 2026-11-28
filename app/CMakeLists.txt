find_package(Threads REQUIRED)
find_package(tomlplusplus 3.4 CONFIG QUIET)
if(NOT TARGET tomlplusplus::tomlplusplus)
  FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tomlplusplus)
endif()

add_library(flock_app_support STATIC
  bot_tools.cpp
  bots.cpp
  chatgpt.cpp
  chatgpt_browser.cpp
  chatgpt_http.cpp
  chatgpt_oauth.cpp
  chatgpt_recording.cpp
  demo.cpp
  git_github.cpp
  model.cpp
  settings_file.cpp
  t3_setup.cpp
)
target_link_libraries(flock_app_support PUBLIC flock_core
                      PRIVATE CURL::libcurl OpenSSL::Crypto Threads::Threads tomlplusplus::tomlplusplus)
target_compile_options(flock_app_support PRIVATE ${flock_warnings})

add_executable(flock
  chatgpt_web.cpp
  main.cpp
  settings_connections.cpp
  settings_system.cpp
  t3_connection_setup.cpp
)
target_link_libraries(flock PRIVATE flock_app_support CURL::libcurl OpenSSL::Crypto)
target_compile_options(flock PRIVATE ${flock_warnings})
set_property(TARGET flock PROPERTY INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS flock)

if(BUILD_TESTING)
  flock_test_executable(flock_app_settings_access SOURCES tests/settings_access.cpp LIBRARIES flock_core)
  add_test(NAME flock_app_settings_access COMMAND flock_app_settings_access)
  flock_test_executable(flock_app_tests SOURCES tests.cpp session_tests.cpp tests/conversation_routing.cpp tests/schedules.cpp LIBRARIES flock_app_support)
  add_test(NAME flock_app COMMAND flock_app_tests)
  flock_test_executable(flock_app_chatgpt_transport SOURCES tests/chatgpt_transport.cpp LIBRARIES flock_app_support)
  add_test(NAME flock_app_chatgpt_transport COMMAND flock_app_chatgpt_transport)
  add_test(NAME flock_app_chatgpt_recordings COMMAND flock_app_chatgpt_transport --recordings
    "${PROJECT_SOURCE_DIR}/tests/recordings/chatgpt.jsonl")
  flock_test_executable(flock_app_chatgpt_oauth SOURCES tests/chatgpt_oauth.cpp chatgpt_web.cpp LIBRARIES flock_app_support flock_web CURL::libcurl OpenSSL::Crypto)
  add_test(NAME flock_app_chatgpt_oauth COMMAND flock_app_chatgpt_oauth)
  flock_test_executable(flock_app_chatgpt_browser SOURCES tests/chatgpt_browser.cpp LIBRARIES flock_app_support)
  add_test(NAME flock_app_chatgpt_browser COMMAND flock_app_chatgpt_browser)
  set_tests_properties(flock_app_chatgpt_oauth flock_app_chatgpt_browser PROPERTIES TIMEOUT 15)
  if(FLOCK_TEST_OPENSSL)
    add_test(NAME flock_app_chatgpt_tls
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_tls.py"
              "$<TARGET_FILE:flock_app_chatgpt_transport>" "${FLOCK_TEST_OPENSSL}")
    set_tests_properties(flock_app_chatgpt_tls PROPERTIES TIMEOUT 15)
  endif()
  flock_test_executable(flock_app_session_settings SOURCES tests/session_settings.cpp LIBRARIES flock_app_support flock_t3)
  add_test(NAME flock_app_session_settings COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/t3/fixture.py" $<TARGET_FILE:flock_app_session_settings>)
  add_test(NAME flock_cli_invalid_port
    COMMAND "${CMAKE_COMMAND}" "-DFLOCK=$<TARGET_FILE:flock>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_port.cmake")
  if(FLOCK_NODE_EXECUTABLE)
    add_test(NAME flock_app_bot_deletion
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot-deletion.mjs"
              "$<TARGET_FILE:flock>" "${Python3_EXECUTABLE}")
    set_tests_properties(flock_app_bot_deletion PROPERTIES TIMEOUT 25)
    add_test(NAME flock_app_project_assignment
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/project-assignment.mjs" "$<TARGET_FILE:flock>")
    set_tests_properties(flock_app_project_assignment PROPERTIES TIMEOUT 25)
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
