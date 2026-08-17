cmake_minimum_required(VERSION 3.16)
project(lsmtree LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(LSM_BUILD_TESTS "Build the GoogleTest suite" ON)
option(LSM_BUILD_BENCH "Build the YCSB-style benchmark" ON)
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
  src/bloom.cpp
  src/cache.cpp
  src/crc32c.cpp
  src/db.cpp
  src/iterator.cpp
  src/memtable.cpp
  src/sstable.cpp
  src/wal.cpp
)
target_include_directories(lsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lsm PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
target_link_libraries(lsm PUBLIC Threads::Threads)

if(LSM_BUILD_BENCH)
  add_executable(lsm_bench bench/bench.cpp)
  target_link_libraries(lsm_bench PRIVATE lsm)
  target_include_directories(lsm_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench)

  # Optional LevelDB baseline. Without it `--engine leveldb` reports that the
  # build lacks the comparison rather than silently measuring nothing.
  option(LSM_BENCH_LEVELDB "Build the benchmark with a LevelDB baseline" OFF)
  if(LSM_BENCH_LEVELDB)
    find_path(LEVELDB_INCLUDE_DIR leveldb/db.h)
    find_library(LEVELDB_LIBRARY NAMES leveldb)
    if(NOT LEVELDB_INCLUDE_DIR OR NOT LEVELDB_LIBRARY)
      message(FATAL_ERROR "LSM_BENCH_LEVELDB=ON but leveldb was not found")
    endif()
    target_compile_definitions(lsm_bench PRIVATE LSM_BENCH_LEVELDB)
    target_include_directories(lsm_bench PRIVATE ${LEVELDB_INCLUDE_DIR})
    target_link_libraries(lsm_bench PRIVATE ${LEVELDB_LIBRARY})
    message(STATUS "LevelDB baseline enabled: ${LEVELDB_LIBRARY}")
  endif()

  add_executable(lsm_crash_worker tools/crash_worker.cpp)
  target_link_libraries(lsm_crash_worker PRIVATE lsm)

  add_executable(lsm_crash_harness tools/crash_harness.cpp)
  target_link_libraries(lsm_crash_harness PRIVATE lsm)
endif()

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
    tests/test_bloom.cpp
    tests/test_cache.cpp
    tests/test_crc32c.cpp
    tests/test_skiplist.cpp
    tests/test_iterator.cpp
    tests/test_memtable.cpp
    tests/test_wal.cpp
    tests/test_sstable.cpp
    tests/test_db.cpp
    tests/test_recovery.cpp
    tests/test_concurrency.cpp
  )
  target_link_libraries(lsm_tests PRIVATE lsm GTest::gtest_main)
  target_compile_options(lsm_tests PRIVATE -Wall -Wextra)

  include(GoogleTest)
  gtest_discover_tests(lsm_tests DISCOVERY_TIMEOUT 120)
endif()
