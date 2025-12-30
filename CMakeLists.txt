cmake_minimum_required(VERSION 3.30)
project(SmallDB)

set(CMAKE_CXX_STANDARD 20)

add_executable(SmallDB main.cpp
        MemTable.cpp
        MemTable.h
        WAL.cpp
        WAL.h
        SSTable.cpp
        SSTable.h
        Slice.h
)
