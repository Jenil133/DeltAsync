cmake_minimum_required(VERSION 3.20)

project(deltasync
  VERSION 0.1.0
  DESCRIPTION "Rolling-hash file synchronization"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
  src/error.cpp
  src/io.cpp
  src/sha256.cpp
)
target_include_directories(deltasync_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(deltasync_core PUBLIC cxx_std_17)
# The sync server runs one thread per connection.
find_package(Threads REQUIRED)
target_link_libraries(deltasync_core PUBLIC Threads::Threads PRIVATE deltasync_warnings)
