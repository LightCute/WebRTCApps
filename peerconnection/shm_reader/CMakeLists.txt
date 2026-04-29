cmake_minimum_required(VERSION 3.14)
project(shm_video_reader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(shm_video_reader
    main.cc
    shm_reader.cc
)

target_include_directories(shm_video_reader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shm_video_reader PRIVATE Threads::Threads)
