cmake_minimum_required(VERSION 3.25)
project(rss_discord_bot VERSION 2.0.0
  DESCRIPTION "Posts RSS/Atom feed items to Discord via webhooks, with a web dashboard"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RSSBOT_BUILD_TESTS "Build the unit tests" ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Use system packages when installed, otherwise download them.
include(FetchContent)
FetchContent_Declare(pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml.git
  GIT_TAG v1.15
  GIT_SHALLOW TRUE
  SYSTEM
  FIND_PACKAGE_ARGS CONFIG)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  SYSTEM
  FIND_PACKAGE_ARGS 3.10 CONFIG)
# cpp-httplib (header-only) serves the dashboard. Plain HTTP only: outgoing
# HTTPS (feeds, Discord) goes through libcurl, so no OpenSSL/zlib needed here.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.64.0.tar.gz
  SYSTEM
  FIND_PACKAGE_ARGS CONFIG)
FetchContent_MakeAvailable(pugixml nlohmann_json httplib)

# Embed web/index.html into the binary (re-run when it changes).
set(RSSBOT_WEB_INDEX ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RSSBOT_WEB_INDEX})
file(READ ${RSSBOT_WEB_INDEX} RSSBOT_DASHBOARD_HTML)
configure_file(src/dashboard_html.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/dashboard_html.hpp @ONLY)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(RSSBOT_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

add_library(rssbot_core STATIC
  src/app.cpp
  src/config.cpp
  src/discord.cpp
  src/feed.cpp
  src/http.cpp
  src/state.cpp
  src/util.cpp)
target_include_directories(rssbot_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rssbot_core PUBLIC CURL::libcurl pugixml::pugixml nlohmann_json::nlohmann_json Threads::Threads)
target_compile_definitions(rssbot_core PUBLIC "RSSBOT_VERSION=\"${PROJECT_VERSION}\"")
target_compile_options(rssbot_core PRIVATE ${RSSBOT_WARNINGS})

add_executable(rss-discord-bot src/main.cpp src/dashboard.cpp)
target_include_directories(rss-discord-bot PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(rss-discord-bot PRIVATE rssbot_core httplib::httplib)
target_compile_options(rss-discord-bot PRIVATE ${RSSBOT_WARNINGS})
install(TARGETS rss-discord-bot RUNTIME DESTINATION bin)

if(RSSBOT_BUILD_TESTS)
  enable_testing()
  add_executable(rssbot_tests tests/test_main.cpp)
  target_link_libraries(rssbot_tests PRIVATE rssbot_core)
  add_test(NAME rssbot_tests COMMAND rssbot_tests)
endif()
