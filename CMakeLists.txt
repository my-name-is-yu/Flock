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

add_library(flock_core STATIC src/database.cpp src/store.cpp src/schema.cpp src/journal.cpp src/task_storage.cpp src/task_context.cpp src/task_interaction.cpp src/wait.cpp src/outbox.cpp src/conversation.cpp src/conversation_agent.cpp src/conversation_actions.cpp src/conversation_projection.cpp
                       src/conversation_dispatch.cpp src/markdown.cpp src/t3_routing.cpp
                       src/workflow.cpp src/context_compaction.cpp src/session_read.cpp src/model_budget.cpp src/channels.cpp src/local_machine.cpp
                       src/runtime.cpp src/runtime_maintenance.cpp src/runtime_task_scheduler.cpp src/runtime_lifecycle.cpp
                       src/runtime_task_turn.cpp
                       src/runtime_task_decisions.cpp
                       src/runtime_outbox_dispatch.cpp src/runtime_outbox_destinations.cpp
                       src/runtime_session_prompts.cpp
                       src/runtime_run_observer.cpp
                       src/runtime_peer_exchanges.cpp
                       src/runtime_run_reports.cpp
                       src/runtime_session_history.cpp
                       src/runtime_task_questions.cpp
                       src/runtime_task_updates.cpp
                       src/run_reports.cpp src/project_slots.cpp src/task_destinations.cpp src/policy.cpp src/policy_approval.cpp src/bot_lifecycle.cpp
                       src/bot_settings_tools.cpp src/identity.cpp src/secrets.cpp src/participation.cpp src/external_mcp.cpp src/skill_export.cpp
                       src/board_inbox.cpp
                       src/memory_store.cpp src/automation.cpp src/cron.cpp
                       src/bot_workspace.cpp src/bot_tools_external.cpp src/parallel_tools.cpp src/tool_batch.cpp src/attachments.cpp src/image_headers.cpp src/base64.cpp
                       src/git_host.cpp src/git_pr.cpp src/git_tools.cpp src/git_webhooks.cpp src/git_followup.cpp)
target_include_directories(flock_core PUBLIC include)
target_sources(flock_core PRIVATE src/bot_profile.cpp)
target_sources(flock_core PRIVATE src/journal_conversation.cpp src/journal_peer.cpp src/journal_task.cpp)
if(TARGET SQLite3::SQLite3)
  target_link_libraries(flock_core PUBLIC SQLite3::SQLite3)
else()
  target_link_libraries(flock_core PUBLIC SQLite::SQLite3)
endif()
target_sources(flock_core PRIVATE src/session_question.cpp src/session_settings.cpp src/traffic_recording.cpp)
target_link_libraries(flock_core PUBLIC nlohmann_json::nlohmann_json cctz::cctz PRIVATE CURL::libcurl OpenSSL::Crypto cmark)
target_compile_options(flock_core PRIVATE -Wall -Wextra -Wpedantic)

option(FLOCK_LIVE_TESTS "Register live ChatGPT and T3 tests in CTest" OFF)
if(BUILD_TESTING)
  add_executable(flock_traffic_recording tests/traffic_recording.cpp)
  target_link_libraries(flock_traffic_recording PRIVATE flock_core)
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

  add_executable(flock_workflow_context tests/workflow_context.cpp)
  target_link_libraries(flock_workflow_context PRIVATE flock_core)
  target_compile_options(flock_workflow_context PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_workflow_context COMMAND flock_workflow_context)
  add_executable(flock_memory_automation tests/memory_automation.cpp)
  target_link_libraries(flock_memory_automation PRIVATE flock_core)
  target_compile_options(flock_memory_automation PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_memory_automation COMMAND flock_memory_automation)
  add_executable(flock_channels_local tests/channels_local.cpp)
  target_link_libraries(flock_channels_local PRIVATE flock_core)
  target_compile_options(flock_channels_local PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_channels_local COMMAND flock_channels_local)
  add_executable(flock_board_inbox tests/board_inbox.cpp)
  target_link_libraries(flock_board_inbox PRIVATE flock_core)
  target_compile_options(flock_board_inbox PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_board_inbox COMMAND flock_board_inbox)
  add_executable(flock_protected_input tests/protected_input.cpp)
  target_link_libraries(flock_protected_input PRIVATE flock_core)
  add_test(NAME flock_protected_input COMMAND flock_protected_input)
  add_executable(flock_supervised_policy tests/supervised_policy.cpp)
  target_link_libraries(flock_supervised_policy PRIVATE flock_core)
  add_test(NAME flock_supervised_policy COMMAND flock_supervised_policy)
  add_executable(flock_session_read tests/session_read.cpp)
  target_link_libraries(flock_session_read PRIVATE flock_core)
  target_compile_options(flock_session_read PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_session_read COMMAND flock_session_read)
  add_executable(flock_core_tests tests/core.cpp)
  target_link_libraries(flock_core_tests PRIVATE flock_core)
  add_test(NAME flock_core COMMAND flock_core_tests)
  add_executable(flock_wait_records tests/wait_records.cpp)
  target_link_libraries(flock_wait_records PRIVATE flock_core)
  add_test(NAME flock_wait_records COMMAND flock_wait_records)
  add_executable(flock_task_records tests/task_records.cpp)
  target_link_libraries(flock_task_records PRIVATE flock_core)
  add_test(NAME flock_task_records COMMAND flock_task_records)
  add_executable(flock_outbox_records tests/outbox.cpp)
  target_link_libraries(flock_outbox_records PRIVATE flock_core)
  add_test(NAME flock_outbox_records COMMAND flock_outbox_records)
  add_executable(flock_core_acceptance tests/core_acceptance.cpp)
  target_link_libraries(flock_core_acceptance PRIVATE flock_core)
  add_test(NAME flock_core_acceptance COMMAND flock_core_acceptance)
  add_executable(flock_conversation_store tests/conversation_store.cpp)
  target_link_libraries(flock_conversation_store PRIVATE flock_core)
  add_test(NAME flock_conversation_store COMMAND flock_conversation_store)
  add_executable(flock_tool_batch tests/tool_batch.cpp)
  target_link_libraries(flock_tool_batch PRIVATE flock_core)
  target_compile_options(flock_tool_batch PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_tool_batch COMMAND flock_tool_batch)
  add_executable(flock_conversation_batch tests/conversation_batch.cpp)
  target_link_libraries(flock_conversation_batch PRIVATE flock_core)
  target_compile_options(flock_conversation_batch PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_conversation_batch COMMAND flock_conversation_batch)
  add_test(NAME flock_tool_batch_measure COMMAND flock_conversation_batch --measure)
  add_executable(flock_markdown_tests tests/markdown.cpp)
  target_link_libraries(flock_markdown_tests PRIVATE flock_core)
  add_test(NAME flock_markdown COMMAND flock_markdown_tests)
  add_executable(flock_image_headers tests/image_headers.cpp)
  target_link_libraries(flock_image_headers PRIVATE flock_core)
  target_compile_options(flock_image_headers PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_image_headers COMMAND flock_image_headers)
  add_executable(flock_conversation_runtime tests/conversation_runtime.cpp)
  target_link_libraries(flock_conversation_runtime PRIVATE flock_core)
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
  add_executable(flock_live_t3 tests/live_t3.cpp)
  target_link_libraries(flock_live_t3 PRIVATE flock_t3)
  add_executable(flock_live_conversation tests/live_conversation.cpp tests/live_conversation_judge.cpp)
  target_include_directories(flock_live_conversation PRIVATE app)
  target_link_libraries(flock_live_conversation PRIVATE flock_app_support)
  add_test(NAME flock_live_conversation_self_test COMMAND flock_live_conversation --self-test)
  add_executable(flock_live_evidence tests/live_evidence.cpp)
  target_include_directories(flock_live_evidence PRIVATE app)
  target_link_libraries(flock_live_evidence PRIVATE flock_t3 flock_app_support)
  add_test(NAME flock_live_evidence_schema COMMAND flock_live_evidence --self-test)
  add_test(NAME flock_verify_live_gate COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_live_gate.py")
  if(FLOCK_LIVE_TESTS)
    foreach(test flock_live_t3 flock_live_conversation flock_live_evidence)
      add_test(NAME ${test} COMMAND ${test} --live)
      set_tests_properties(${test} PROPERTIES LABELS live SKIP_RETURN_CODE 77 RESOURCE_LOCK flock_live)
    endforeach()
    set_tests_properties(flock_live_t3 PROPERTIES TIMEOUT 360)
    set_tests_properties(flock_live_conversation flock_live_evidence PROPERTIES TIMEOUT 1500)
  endif()
  add_executable(flock_git_pr_ci tests/git_pr_ci.cpp tests/git_pr_followup.cpp)
  target_include_directories(flock_git_pr_ci PRIVATE app)
  target_link_libraries(flock_git_pr_ci PRIVATE flock_app_support)
  target_compile_options(flock_git_pr_ci PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_git_pr_ci COMMAND flock_git_pr_ci)
  add_executable(flock_git_bot_deletion tests/git_bot_deletion.cpp)
  target_link_libraries(flock_git_bot_deletion PRIVATE flock_core)
  target_compile_options(flock_git_bot_deletion PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_git_bot_deletion COMMAND flock_git_bot_deletion)
  add_executable(flock_bot_tools tests/bot_tools.cpp)
  target_include_directories(flock_bot_tools PRIVATE app)
  target_link_libraries(flock_bot_tools PRIVATE flock_app_support)
  target_compile_options(flock_bot_tools PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME flock_bot_tools COMMAND flock_bot_tools)
  add_test(NAME flock_tool_history_projection COMMAND flock_bot_tools --tool-history)
  add_test(NAME flock_bot_tools_mcp COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/bot_tools_mcp.py" $<TARGET_FILE:flock_bot_tools>)
endif()
