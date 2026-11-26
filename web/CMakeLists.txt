find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

FetchContent_Declare(flock_httplib
  URL https://codeload.github.com/yhirose/cpp-httplib/tar.gz/refs/tags/v0.67.0
  URL_HASH SHA256=be711ddaf7a3655d8f23aa5655709250b24d247aedd4b2629989d18b6164e5be
  SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(flock_httplib)

file(GLOB_RECURSE flock_web_assets CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
set(flock_web_asset_header "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.hpp")
add_custom_command(OUTPUT "${flock_web_asset_header}"
  COMMAND "${CMAKE_COMMAND}"
    "-Dasset_dir=${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "-Doutput=${flock_web_asset_header}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake"
  DEPENDS ${flock_web_assets} "${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake"
  VERBATIM)

add_library(flock_web STATIC server.cpp settings_routes.cpp board_inbox_routes.cpp memory_automation_routes.cpp git_routes.cpp identity.cpp views.cpp change_feed.cpp
                      channel_routes.cpp channel_bridge.cpp mcp_routes.cpp "${flock_web_asset_header}")
target_include_directories(flock_web PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(flock_web SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
target_link_libraries(flock_web PUBLIC flock_core PRIVATE Threads::Threads OpenSSL::Crypto)
target_compile_options(flock_web PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_executable(flock_web_git_webhooks tests/git_webhooks.cpp)
  target_include_directories(flock_web_git_webhooks SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
  target_link_libraries(flock_web_git_webhooks PRIVATE flock_web flock_git_test_sessions Threads::Threads OpenSSL::Crypto)
  target_compile_options(flock_web_git_webhooks PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_web_git_webhooks COMMAND flock_web_git_webhooks)
  if(FLOCK_NODE_EXECUTABLE AND FLOCK_CHROME_EXECUTABLE)
    add_test(NAME flock_web_browser_notification_navigation COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/browser-notification-navigation.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    add_test(NAME flock_web_bot_access_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot-access.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    add_test(NAME flock_web_bot_pause_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot-pause.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    add_test(NAME flock_web_bot_settings_switch_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot-settings-switch.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    set_tests_properties(flock_web_bot_settings_switch_dom PROPERTIES TIMEOUT 60)
    add_test(NAME flock_web_board_inbox_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/board-inbox.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    add_test(NAME flock_web_board_inbox_live_retry COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/board-inbox.mjs" "${FLOCK_CHROME_EXECUTABLE}" live)
    add_test(NAME flock_web_composer_screenshot_resize COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/composer-screenshot-resize.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    set_tests_properties(flock_web_composer_screenshot_resize PROPERTIES TIMEOUT 30)
    add_test(NAME flock_web_message_time_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/message-time.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    set_tests_properties(flock_web_message_time_dom PROPERTIES TIMEOUT 30)
    add_test(NAME flock_web_question_choices_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/question-choices.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    add_test(NAME flock_web_conversation_find_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/conversation-find.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    add_test(NAME flock_web_recurring_requests_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/recurring-requests.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    add_test(NAME flock_web_bot_workspace_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot-workspace.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    set_tests_properties(flock_web_bot_access_dom flock_web_bot_pause_dom flock_web_board_inbox_dom flock_web_board_inbox_live_retry
      flock_web_question_choices_dom flock_web_conversation_find_dom
      flock_web_recurring_requests_dom flock_web_bot_workspace_dom PROPERTIES TIMEOUT 30)
  endif()
  add_executable(flock_web_tests tests/http.cpp tests/board_inbox.cpp tests/channels_local.cpp)
  target_include_directories(flock_web_tests SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
  target_link_libraries(flock_web_tests PRIVATE flock_web Threads::Threads)
  add_test(NAME flock_web COMMAND flock_web_tests)
  add_executable(flock_web_conversation_tests tests/conversation.cpp)
  target_include_directories(flock_web_conversation_tests SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
  target_include_directories(flock_web_conversation_tests PRIVATE "${PROJECT_SOURCE_DIR}/app")
  target_link_libraries(flock_web_conversation_tests PRIVATE flock_web flock_app_support Threads::Threads)
  add_test(NAME flock_web_conversation COMMAND flock_web_conversation_tests)
  add_test(NAME flock_web_chatgpt_conversation COMMAND flock_web_conversation_tests --chatgpt)
  set_tests_properties(flock_web_chatgpt_conversation PROPERTIES TIMEOUT 30)
  add_executable(flock_web_session_mcp_tests tests/session_mcp.cpp)
  target_include_directories(flock_web_session_mcp_tests SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
  target_link_libraries(flock_web_session_mcp_tests PRIVATE flock_web flock_t3 Threads::Threads)
  target_compile_options(flock_web_session_mcp_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_web_session_mcp COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/t3/fixture.py" $<TARGET_FILE:flock_web_session_mcp_tests>)
  add_executable(flock_web_external_mcp_tests tests/external_mcp.cpp)
  target_include_directories(flock_web_external_mcp_tests SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
  target_link_libraries(flock_web_external_mcp_tests PRIVATE flock_web Threads::Threads)
  target_compile_options(flock_web_external_mcp_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_web_external_mcp COMMAND flock_web_external_mcp_tests)
  set_tests_properties(flock_web_external_mcp PROPERTIES TIMEOUT 30)
  if(FLOCK_NODE_EXECUTABLE)
    add_test(NAME flock_web_chat_grouping
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/chat-grouping.mjs")
    add_test(NAME flock_web_find_match_time
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/find-match-time.mjs")
    add_test(NAME flock_web_action_labels
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/action-labels.mjs")
    add_test(NAME flock_web_composer_drafts
      COMMAND "${FLOCK_NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/tests/composer-drafts.mjs")
    add_test(NAME flock_web_event_history
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/event-history.mjs")
    add_test(NAME flock_web_composer_screenshots
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/composer-screenshots.mjs")
    if(FLOCK_CHROME_EXECUTABLE)
      add_test(NAME flock_web_timeline_rows_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/timeline-rows.mjs" "${FLOCK_CHROME_EXECUTABLE}")
      set_tests_properties(flock_web_timeline_rows_dom PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME flock_web_composer_shortcuts
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/composer-shortcuts.mjs")
    add_test(NAME flock_web_browser_notifications
      COMMAND "${FLOCK_NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/tests/browser-notifications.mjs")
  endif()
endif()
