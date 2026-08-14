cmake_minimum_required(VERSION 3.16)
project(lsmtree LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(LSM_BUILD_TESTS "Build the GoogleTest suite" ON)
option(LSM_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LSM_TSAN "Build with ThreadSanitizer" OFF)

# Sanitizers are mutually exclusive; CI runs them as separate jobs. These have
# to be set before any target is defined -- add_compile_options only applies to
# targets created after the call.
if(LSM_ASAN AND LSM_TSAN)
  message(FATAL_ERROR "LSM_ASAN and LSM_TSAN cannot be enabled together")
endif()
if(LSM_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
if(LSM_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

add_library(lsm
  src/arena.cpp
  src/crc32c.cpp
)
target_include_directories(lsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lsm PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
target_link_libraries(lsm PUBLIC Threads::Threads)

if(LSM_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lsm_tests
    tests/test_arena.cpp
    tests/test_crc32c.cpp
  )
  target_link_libraries(lsm_tests PRIVATE lsm GTest::gtest_main)
  target_compile_options(lsm_tests PRIVATE -Wall -Wextra)

  include(GoogleTest)
  gtest_discover_tests(lsm_tests DISCOVERY_TIMEOUT 120)
endif()
