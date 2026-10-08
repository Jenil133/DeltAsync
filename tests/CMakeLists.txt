include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_library(deltasync_test_support STATIC
  edit_model.cpp
  test_util.cpp
)
target_link_libraries(deltasync_test_support
  PUBLIC deltasync_core GTest::gtest
  PRIVATE deltasync_warnings)

# Default suite: fast unit and integration tests.
add_executable(deltasync_tests
  bytes_test.cpp
  delta_encoder_test.cpp
  delta_format_test.cpp
  file_test.cpp
  patch_test.cpp
  rolling_checksum_test.cpp
  sha256_test.cpp
  signature_test.cpp
)
target_link_libraries(deltasync_tests PRIVATE
  deltasync_test_support GTest::gtest_main deltasync_warnings)
gtest_discover_tests(deltasync_tests
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 120
  PROPERTIES LABELS unit TIMEOUT 300)
