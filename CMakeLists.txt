cmake_minimum_required(VERSION 3.24)
project(flock VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)
include(FetchContent)
include(CTest)
option(FLOCK_REQUIRE_ALL_TESTS "Require tools for every Flock test group" OFF)
if(BUILD_TESTING)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  find_program(FLOCK_NODE_EXECUTABLE node NO_CACHE)
  find_program(FLOCK_CHROME_EXECUTABLE
    NAMES chromium chromium-browser google-chrome "Google Chrome" Chromium
    PATHS "/Applications/Google Chrome.app/Contents/MacOS" "/Applications/Chromium.app/Contents/MacOS"
    NO_CACHE)
  find_program(FLOCK_TEST_OPENSSL openssl NO_CACHE)

  if(NOT FLOCK_NODE_EXECUTABLE)
    list(APPEND flock_skipped_test_groups "Node.js and browser DOM tests (node not found)")
  endif()
  if(NOT FLOCK_CHROME_EXECUTABLE)
    list(APPEND flock_skipped_test_groups "browser DOM tests (Chrome/Chromium not found)")
  endif()
  if(NOT FLOCK_TEST_OPENSSL)
    list(APPEND flock_skipped_test_groups "ChatGPT TLS test (openssl CLI not found)")
  endif()
  if(flock_skipped_test_groups)
    list(JOIN flock_skipped_test_groups "\n  " flock_skipped_test_groups)
    if(FLOCK_REQUIRE_ALL_TESTS)
      message(FATAL_ERROR "FLOCK_REQUIRE_ALL_TESTS=ON cannot skip test groups:\n  ${flock_skipped_test_groups}")
    endif()
    message(STATUS "Skipping Flock test groups:\n  ${flock_skipped_test_groups}")
  endif()
endif()

find_package(SQLite3 3.35 REQUIRED)
find_package(CURL 7.85 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json 3.12 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(nlohmann_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
    URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

function(flock_fetch_cctz)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  set(BUILD_TOOLS OFF)
  set(BUILD_EXAMPLES OFF)
  FetchContent_Declare(cctz
    URL https://codeload.github.com/google/cctz/tar.gz/refs/tags/v2.5
    URL_HASH SHA256=47d2d68e7cb5af3296dc7e69b0f4a765589f1b2f4af4b9c42e772414c428b421)
  FetchContent_MakeAvailable(cctz)
  set_property(DIRECTORY "${cctz_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
endfunction()
flock_fetch_cctz()

function(flock_fetch_cmark)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  FetchContent_Declare(flock_cmark
    URL https://codeload.github.com/commonmark/cmark/tar.gz/refs/tags/0.31.1
    URL_HASH SHA256=3da93db5469c30588cfeb283d9d62edfc6ded9eb0edc10a4f5bbfb7d722ea802)
  FetchContent_MakeAvailable(flock_cmark)
  set_property(DIRECTORY "${flock_cmark_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
endfunction()
flock_fetch_cmark()

option(FLOCK_WARNINGS_AS_ERRORS "Fail the build on a compiler warning in Flock code" OFF)
set(flock_warnings -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
if(FLOCK_WARNINGS_AS_ERRORS)
  list(APPEND flock_warnings -Werror)
endif()

add_library(flock_core STATIC
  src/attachments.cpp
  src/automation.cpp
  src/base64.cpp
  src/board_inbox.cpp
  src/bot_lifecycle.cpp
  src/bot_profile.cpp
  src/bot_settings_tools.cpp
  src/bot_setup.cpp
  src/bot_tools_external.cpp
  src/bot_workspace.cpp
  src/channels.cpp
  src/context_compaction.cpp
  src/conversation.cpp
  src/conversation_actions.cpp
  src/conversation_agent.cpp
  src/conversation_dispatch.cpp
  src/conversation_projection.cpp
  src/cron.cpp
  src/database.cpp
  src/digest.cpp
  src/external_mcp.cpp
  src/git_followup.cpp
  src/git_host.cpp
  src/git_pr.cpp
  src/git_tools.cpp
  src/git_webhooks.cpp
  src/identity.cpp
  src/image_headers.cpp
  src/journal.cpp
  src/journal_conversation.cpp
  src/journal_git.cpp
  src/journal_task.cpp
  src/local_machine.cpp
  src/markdown.cpp
  src/memory_learnings.cpp
  src/memory_store.cpp
  src/model_calls.cpp
  src/outbox.cpp
  src/parallel_tools.cpp
  src/participation.cpp
  src/percent_encoding.cpp
  src/policy.cpp
  src/policy_approval.cpp
  src/project_slots.cpp
  src/run_reports.cpp
  src/runtime.cpp
  src/runtime_lifecycle.cpp
  src/runtime_maintenance.cpp
  src/runtime_outbox_destinations.cpp
  src/runtime_outbox_dispatch.cpp
  src/runtime_run_observer.cpp
  src/runtime_run_reports.cpp
  src/runtime_session_prompts.cpp
  src/runtime_task_decisions.cpp
  src/runtime_task_questions.cpp
  src/runtime_task_scheduler.cpp
  src/runtime_task_turn.cpp
  src/runtime_task_updates.cpp
  src/schema.cpp
  src/secrets.cpp
  src/session_question.cpp
  src/session_read.cpp
  src/session_settings.cpp
  src/settings_access.cpp
  src/store.cpp
  src/t3.cpp
  src/t3_routing.cpp
  src/task_context.cpp
  src/task_destinations.cpp
  src/task_interaction.cpp
  src/task_storage.cpp
  src/tool_batch.cpp
  src/traffic_recording.cpp
  src/wait.cpp
)
target_include_directories(flock_core PUBLIC include)
if(TARGET SQLite3::SQLite3)
  target_link_libraries(flock_core PUBLIC SQLite3::SQLite3)
else()
  target_link_libraries(flock_core PUBLIC SQLite::SQLite3)
endif()
target_link_libraries(flock_core PUBLIC nlohmann_json::nlohmann_json cctz::cctz PRIVATE CURL::libcurl OpenSSL::Crypto cmark)
target_compile_options(flock_core PRIVATE ${flock_warnings})

function(flock_test_executable target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBRARIES")
  add_executable(${target} ${arg_SOURCES})
  target_link_libraries(${target} PRIVATE ${arg_LIBRARIES})
  target_compile_options(${target} PRIVATE ${flock_warnings})
endfunction()

option(FLOCK_LIVE_TESTS "Register live ChatGPT and T3 tests in CTest" OFF)
if(BUILD_TESTING)
  add_library(flock_git_test_sessions STATIC tests/fake_git_sessions.cpp)
  target_link_libraries(flock_git_test_sessions PUBLIC flock_core)

  flock_test_executable(flock_traffic_recording SOURCES tests/traffic_recording.cpp LIBRARIES flock_core)
  add_test(NAME flock_traffic_recording COMMAND flock_traffic_recording)

  function(flock_verify_model name skill)
    set(model "${PROJECT_SOURCE_DIR}/.agents/skills/${skill}/model.py")
    add_test(NAME "flock_verify_${name}" COMMAND "${Python3_EXECUTABLE}" "${model}")
  endfunction()
  function(flock_replay_model name skill variant expected)
    add_test(NAME "flock_verify_${name}" COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/verify-model-replay.py"
      "${PROJECT_SOURCE_DIR}/.agents/skills/${skill}/model.py" "${variant}" "${expected}")
  endfunction()
  flock_verify_model(reply_motion verify-reply-motion)
  flock_replay_model(reply_motion_historical verify-reply-motion --historical
    "A catch-up refresh animated a reply")
  flock_replay_model(reply_motion_ignored_catch_up verify-reply-motion --ignore-catch-up
    "A catch-up refresh animated a reply")
  flock_verify_model(change_feed verify-change-feed)
  flock_replay_model(change_feed_historical verify-change-feed --historical
    "The Bot view recovered, but the history invalidation was lost")
  flock_verify_model(run_settlement verify-run-settlement)
  flock_replay_model(run_settlement_historical verify-run-settlement --historical
    "Flock released the Run while its delegated work was active")
  flock_verify_model(automation_routing verify-automation-routing)
  flock_replay_model(automation_workspace_historical verify-automation-routing --historical=workspace
    "A configured occurrence used the old workspace after the new workspace was published")
  foreach(entry scheduler webhook)
    flock_replay_model("automation_${entry}_historical" verify-automation-routing "--historical=${entry}"
      "An unreadable Bot profile blocked another Bot's automation delivery")
  endforeach()
  flock_replay_model(automation_notes_historical verify-automation-routing --historical=notes
    "An unreadable Bot's notes wait held the shared project slot")
  flock_replay_model(automation_notes_retry_historical verify-automation-routing --historical=notes-retry
    "A retried notes wait overtook work already active in the shared project")
  flock_replay_model(automation_notes_edit_historical verify-automation-routing --historical=notes-edit
    "An edited automation stayed outside the queue after the project became idle")

  flock_test_executable(flock_conversation_context SOURCES tests/conversation_context.cpp LIBRARIES flock_core)
  add_test(NAME flock_conversation_context COMMAND flock_conversation_context)
  flock_test_executable(flock_memory_automation SOURCES tests/memory_automation.cpp LIBRARIES flock_core)
  add_test(NAME flock_memory_automation COMMAND flock_memory_automation)
  flock_test_executable(flock_channels_local SOURCES tests/channels_local.cpp LIBRARIES flock_core)
  add_test(NAME flock_channels_local COMMAND flock_channels_local)
  flock_test_executable(flock_board_inbox SOURCES tests/board_inbox.cpp LIBRARIES flock_core)
  add_test(NAME flock_board_inbox COMMAND flock_board_inbox)
  flock_test_executable(flock_protected_input SOURCES tests/protected_input.cpp LIBRARIES flock_core)
  add_test(NAME flock_protected_input COMMAND flock_protected_input)
  flock_test_executable(flock_supervised_policy SOURCES tests/supervised_policy.cpp LIBRARIES flock_core)
  add_test(NAME flock_supervised_policy COMMAND flock_supervised_policy)
  flock_test_executable(flock_session_read SOURCES tests/session_read.cpp LIBRARIES flock_core)
  add_test(NAME flock_session_read COMMAND flock_session_read)
  flock_test_executable(flock_core_tests SOURCES tests/core.cpp LIBRARIES flock_core)
  add_test(NAME flock_core COMMAND flock_core_tests)
  flock_test_executable(flock_wait_records SOURCES tests/wait_records.cpp LIBRARIES flock_core)
  add_test(NAME flock_wait_records COMMAND flock_wait_records)
  flock_test_executable(flock_task_records SOURCES tests/task_records.cpp LIBRARIES flock_core)
  add_test(NAME flock_task_records COMMAND flock_task_records)
  flock_test_executable(flock_outbox_records SOURCES tests/outbox.cpp LIBRARIES flock_core)
  add_test(NAME flock_outbox_records COMMAND flock_outbox_records)
  flock_test_executable(flock_core_acceptance SOURCES tests/core_acceptance.cpp LIBRARIES flock_core)
  add_test(NAME flock_core_acceptance COMMAND flock_core_acceptance)
  flock_test_executable(flock_conversation_store SOURCES tests/conversation_store.cpp LIBRARIES flock_core)
  add_test(NAME flock_conversation_store COMMAND flock_conversation_store)
  flock_test_executable(flock_tool_batch SOURCES tests/tool_batch.cpp LIBRARIES flock_core)
  add_test(NAME flock_tool_batch COMMAND flock_tool_batch)
  flock_test_executable(flock_conversation_batch SOURCES tests/conversation_batch.cpp LIBRARIES flock_core)
  add_test(NAME flock_conversation_batch COMMAND flock_conversation_batch)
  add_test(NAME flock_tool_batch_measure COMMAND flock_conversation_batch --measure)
  flock_test_executable(flock_markdown_tests SOURCES tests/markdown.cpp LIBRARIES flock_core)
  add_test(NAME flock_markdown COMMAND flock_markdown_tests)
  flock_test_executable(flock_image_headers SOURCES tests/image_headers.cpp LIBRARIES flock_core)
  add_test(NAME flock_image_headers COMMAND flock_image_headers)
  flock_test_executable(flock_conversation_runtime SOURCES tests/conversation_runtime.cpp LIBRARIES flock_core)
  add_test(NAME flock_conversation_runtime COMMAND flock_conversation_runtime)
endif()

add_subdirectory(t3)
add_subdirectory(app)
add_subdirectory(web)
target_link_libraries(flock PRIVATE flock_web flock_t3)

if(BUILD_TESTING)
  set(schema_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/update-schema-snapshot")
  add_test(NAME flock_schema_snapshot COMMAND "${Python3_EXECUTABLE}"
    "${schema_script}" --check $<TARGET_FILE:flock>)
  add_test(NAME flock_schema_open COMMAND "${Python3_EXECUTABLE}"
    "${schema_script}" --concurrent $<TARGET_FILE:flock>)
  add_test(NAME flock_t3_connection_setup COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/t3/fixture.py" $<TARGET_FILE:flock> connection-setup)
  set_tests_properties(flock_t3_connection_setup PROPERTIES TIMEOUT 45)
  flock_test_executable(flock_live_t3 SOURCES tests/live_t3.cpp LIBRARIES flock_t3)
  flock_test_executable(flock_live_conversation SOURCES tests/live_conversation.cpp tests/live_conversation_judge.cpp LIBRARIES flock_app_support)
  target_include_directories(flock_live_conversation PRIVATE app)
  add_test(NAME flock_live_conversation_self_test COMMAND flock_live_conversation --self-test)
  add_test(NAME flock_verify_live_gate COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_live_gate.py")
  add_test(NAME flock_header_fanout COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/header_fanout.py")
  add_test(NAME flock_architecture COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-architecture.py" "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME flock_architecture_check COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture.py")
  if(FLOCK_LIVE_TESTS)
    foreach(test flock_live_t3 flock_live_conversation)
      add_test(NAME ${test} COMMAND ${test} --live)
      set_tests_properties(${test} PROPERTIES LABELS live SKIP_RETURN_CODE 77 RESOURCE_LOCK flock_live)
    endforeach()
    set_tests_properties(flock_live_t3 PROPERTIES TIMEOUT 360)
    set_tests_properties(flock_live_conversation PROPERTIES TIMEOUT 1500)
  endif()
  flock_test_executable(flock_git_pr_ci SOURCES tests/git_pr_ci.cpp tests/git_pr_followup.cpp LIBRARIES flock_app_support flock_git_test_sessions)
  target_include_directories(flock_git_pr_ci PRIVATE app)
  add_test(NAME flock_git_pr_ci COMMAND flock_git_pr_ci)
  flock_test_executable(flock_git_bot_deletion SOURCES tests/git_bot_deletion.cpp LIBRARIES flock_git_test_sessions)
  add_test(NAME flock_git_bot_deletion COMMAND flock_git_bot_deletion)
  flock_test_executable(flock_bot_tools SOURCES tests/bot_tools.cpp LIBRARIES flock_app_support)
  target_include_directories(flock_bot_tools PRIVATE app)
  add_test(NAME flock_bot_tools COMMAND flock_bot_tools)
  add_test(NAME flock_tool_history_projection COMMAND flock_bot_tools --tool-history)
  add_test(NAME flock_bot_tools_mcp COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot_tools_mcp.py" $<TARGET_FILE:flock_bot_tools>)
endif()
