find_package(Threads REQUIRED)

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

add_library(flock_web server.cpp access_routes.cpp board_inbox_routes.cpp identity.cpp views.cpp change_feed.cpp "${flock_web_asset_header}")
target_include_directories(flock_web PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(flock_web SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
target_link_libraries(flock_web PUBLIC flock_core PRIVATE Threads::Threads)
target_compile_options(flock_web PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  find_program(FLOCK_NODE_EXECUTABLE node)
  find_program(FLOCK_CHROME_EXECUTABLE NAMES chromium chromium-browser google-chrome "Google Chrome"
    PATHS "/Applications/Google Chrome.app/Contents/MacOS")
  if(FLOCK_NODE_EXECUTABLE AND FLOCK_CHROME_EXECUTABLE)
    add_test(NAME flock_web_board_inbox_dom COMMAND "${FLOCK_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/board-inbox.mjs" "${FLOCK_CHROME_EXECUTABLE}")
    set_tests_properties(flock_web_board_inbox_dom PROPERTIES TIMEOUT 30)
  endif()
  add_executable(flock_web_tests tests/http.cpp tests/board_inbox.cpp)
  target_include_directories(flock_web_tests SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
  target_link_libraries(flock_web_tests PRIVATE flock_web Threads::Threads)
  add_test(NAME flock_web COMMAND flock_web_tests)
  add_executable(flock_web_conversation_tests tests/conversation.cpp)
  target_include_directories(flock_web_conversation_tests SYSTEM PRIVATE "${flock_httplib_SOURCE_DIR}")
  target_link_libraries(flock_web_conversation_tests PRIVATE flock_web Threads::Threads)
  add_test(NAME flock_web_conversation COMMAND flock_web_conversation_tests)
  if(FLOCK_NODE_EXECUTABLE)
    add_test(NAME flock_web_event_history
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/event-history.mjs")
    if(TARGET flock_app_support)
      target_include_directories(flock_web_conversation_tests PRIVATE "${PROJECT_SOURCE_DIR}/app")
      target_link_libraries(flock_web_conversation_tests PRIVATE flock_app_support)
      target_compile_definitions(flock_web_conversation_tests PRIVATE FLOCK_TEST_CHATGPT=1)
      add_test(NAME flock_web_chatgpt_conversation
        COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt.mjs"
          "$<TARGET_FILE:flock_web_conversation_tests>")
      set_tests_properties(flock_web_chatgpt_conversation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    endif()
  endif()
endif()
