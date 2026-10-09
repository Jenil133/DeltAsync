cmake_minimum_required(VERSION 3.20)

project(deltasync
  VERSION 0.1.0
  DESCRIPTION "Rolling-hash file synchronization"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DELTASYNC_BUILD_TESTS "Build the test suite" ON)
set(DELTASYNC_SANITIZE "" CACHE STRING
  "Comma-separated sanitizers to build with, e.g. 'address,undefined' or 'thread'")

if(DELTASYNC_SANITIZE)
  # Applied to every target, including GoogleTest, so instrumented and
  # uninstrumented code never share containers or allocators.
  add_compile_options(-fsanitize=${DELTASYNC_SANITIZE} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${DELTASYNC_SANITIZE})
endif()

# Warning flags are attached to project targets only; third-party code keeps
# its own settings.
add_library(deltasync_warnings INTERFACE)
target_compile_options(deltasync_warnings INTERFACE -Wall -Wextra -Wpedantic)

add_library(deltasync_core STATIC
  src/bytes.cpp
  src/block_index.cpp
  src/delta_encoder.cpp
  src/delta_format.cpp
  src/error.cpp
  src/file.cpp
  src/io.cpp
  src/net.cpp
  src/patch.cpp
  src/protocol.cpp
  src/resume.cpp
  src/sha256.cpp
  src/signature.cpp
  src/sync_client.cpp
  src/sync_server.cpp
)
target_include_directories(deltasync_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(deltasync_core PUBLIC cxx_std_17)
# The sync server runs one thread per connection.
find_package(Threads REQUIRED)
target_link_libraries(deltasync_core PUBLIC Threads::Threads PRIVATE deltasync_warnings)

# The command-line front end is a library of its own so tests can exercise
# its helpers directly as well as through the executable.
add_library(deltasync_cli STATIC tools/cli.cpp)
target_include_directories(deltasync_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_compile_definitions(deltasync_cli PRIVATE DELTASYNC_VERSION="${PROJECT_VERSION}")
target_link_libraries(deltasync_cli PUBLIC deltasync_core PRIVATE deltasync_warnings)

add_executable(deltasync tools/main.cpp)
target_link_libraries(deltasync PRIVATE deltasync_cli deltasync_warnings)

# Fault-injection TCP forwarder used by the resume tests and the netem
# benchmark scripts to interrupt real syncs.
find_package(Threads REQUIRED)
add_executable(deltasync_proxy tools/flaky_proxy.cpp)
target_link_libraries(deltasync_proxy PRIVATE deltasync_core Threads::Threads deltasync_warnings)

if(DELTASYNC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
