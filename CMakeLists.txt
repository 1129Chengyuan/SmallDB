cmake_minimum_required(VERSION 3.30)
project(SmallDB)

set(CMAKE_CXX_STANDARD 20)

add_executable(SmallDB main.cpp
        mem_table.cpp
        mem_table.h
        WAL.cpp
        WAL.h
        ss_table.cpp
        ss_table.h
        Slice.h
        compaction.cpp
        compaction.h
        smalldb.cpp
        smalldb.h
)
