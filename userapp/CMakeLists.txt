cmake_minimum_required(VERSION 3.22)
project(RanEngineDPDK CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -O3 -march=native -pthread)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

include_directories(${DPDK_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR})
link_directories(${DPDK_LIBRARY_DIRS})

add_executable(ran_engine dpdk-process-engine.cpp)
target_link_libraries(ran_engine PRIVATE ${DPDK_LIBRARIES})
