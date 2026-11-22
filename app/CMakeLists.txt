# libc++ on supported macOS versions does not ship the C++20 time-zone library.
set(BUILD_TZ_LIB ON CACHE BOOL "Build the IANA time-zone library")
set(USE_SYSTEM_TZ_DB ON CACHE BOOL "Read the host's IANA time-zone database")
set(ENABLE_DATE_INSTALL OFF CACHE BOOL "Do not install the date dependency separately")
FetchContent_Declare(date
  URL https://codeload.github.com/HowardHinnant/date/tar.gz/refs/tags/v3.0.4
  URL_HASH SHA256=56e05531ee8994124eeb498d0e6a5e1c3b9d4fccbecdf555fe266631368fb55f)
FetchContent_MakeAvailable(date)

configure_file(chatgpt.mjs chatgpt.mjs COPYONLY)
find_package(CURL REQUIRED)
add_library(flock_app_support bots.cpp model.cpp git_github.cpp)
target_link_libraries(flock_app_support PUBLIC flock_core date::date-tz PRIVATE CURL::libcurl)
target_compile_definitions(flock_app_support PRIVATE FLOCK_CHATGPT_BRIDGE="${CMAKE_CURRENT_BINARY_DIR}/chatgpt.mjs")
target_compile_options(flock_app_support PRIVATE -Wall -Wextra -Wpedantic)

add_executable(flock main.cpp demo.cpp)
target_link_libraries(flock PRIVATE flock_app_support)
target_compile_options(flock PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_executable(flock_app_tests tests.cpp demo.cpp)
  target_link_libraries(flock_app_tests PRIVATE flock_app_support)
  add_test(NAME flock_app COMMAND flock_app_tests)
  find_program(FLOCK_NODE_EXECUTABLE node)
  if(FLOCK_NODE_EXECUTABLE)
    add_test(NAME flock_app_login
      COMMAND "${FLOCK_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/login.mjs" "$<TARGET_FILE:flock>")
    set_tests_properties(flock_app_login PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 25)
  endif()
endif()
