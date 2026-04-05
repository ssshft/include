cmake_minimum_required(VERSION 3.6)
project(shmqueue)

set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -f -std=c++11")
set(CMAKE_BUILD_TYPE "Debug")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -fPIC -Wall -g -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -fPIC -Wall")
include_directories(
        .
        ..
        /opt/version/3rd/
        )
set(SOURCE_FILES
        shmmqueue.cpp
        shmmqueue.h
        shm_rwlock.h
        shm_rwlock.cpp)
add_executable(shmqueue src/main.cpp ${SOURCE_FILES})
add_executable(shmqueue_writer src/shmqueue_writer.cpp ${SOURCE_FILES})
add_executable(shmqueue_reader src/shmqueue_reader.cpp ${SOURCE_FILES})
target_link_libraries(shmqueue pthread)
