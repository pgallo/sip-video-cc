cmake_minimum_required(VERSION 3.16)
project(sip_video_cc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SVCC_BUILD_TESTS "Build unit tests" ON)

add_library(svcc
  src/sip.cpp
  src/sdp.cpp
  src/rtp.cpp
  src/rtcp.cpp
  src/media.cpp
  src/xml.cpp
  src/csta.cpp
  src/csta_net.cpp
  src/phone.cpp
  src/contact_center.cpp
  src/pbx.cpp
  src/desktop.cpp
)
target_include_directories(svcc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(svcc PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(svcc_demo app/demo.cpp)
target_link_libraries(svcc_demo PRIVATE svcc)

if(SVCC_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(svcc_tests
    tests/test_sdp.cpp
    tests/test_rtp.cpp
    tests/test_rtcp.cpp
    tests/test_csta.cpp
    tests/test_contact_center.cpp
    tests/test_end_to_end.cpp
  )
  target_link_libraries(svcc_tests PRIVATE svcc GTest::gtest GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(svcc_tests)

  # The demo is also a regression gate: it exits non-zero if its own checks fail.
  add_test(NAME demo_scenario COMMAND svcc_demo --quiet)
endif()
