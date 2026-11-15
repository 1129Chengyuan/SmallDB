cmake_minimum_required(VERSION 3.30)
project(SmallDB)

set(CMAKE_CXX_STANDARD 20)

# Benchmarks are meaningless in Debug; default to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Storage engine as a library so the benchmark and the tests share one build.
# File names must match case exactly: Linux filesystems are case-sensitive.
add_library(smalldb_core STATIC
        mem_table.cpp
        wal.cpp
        ss_table.cpp
        compaction.cpp
        smalldb.cpp
        bloom_filter.cpp
)
target_include_directories(smalldb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(smalldb_core PUBLIC Threads::Threads)

add_executable(SmallDB main.cpp)
target_link_libraries(SmallDB PRIVATE smalldb_core)

# Correctness tests: ctest --test-dir build
enable_testing()
add_executable(smalldb_tests tests/smalldb_test.cpp)
target_link_libraries(smalldb_tests PRIVATE smalldb_core)
add_test(NAME smalldb_tests COMMAND smalldb_tests)

# Optional RocksDB comparison (brew install rocksdb)
option(WITH_ROCKSDB "Benchmark against RocksDB" ON)
if(WITH_ROCKSDB)
    find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h HINTS /opt/homebrew/opt/rocksdb/include)
    find_library(ROCKSDB_LIBRARY rocksdb HINTS /opt/homebrew/opt/rocksdb/lib)
    if(ROCKSDB_INCLUDE_DIR AND ROCKSDB_LIBRARY)
        message(STATUS "RocksDB found: ${ROCKSDB_LIBRARY}")
        target_include_directories(SmallDB PRIVATE ${ROCKSDB_INCLUDE_DIR})
        target_link_libraries(SmallDB PRIVATE ${ROCKSDB_LIBRARY})
        target_compile_definitions(SmallDB PRIVATE SMALLDB_WITH_ROCKSDB)
    else()
        message(WARNING "RocksDB not found; benchmarking SmallDB only")
    endif()
endif()
