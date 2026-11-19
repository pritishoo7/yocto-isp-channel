cmake_minimum_required(VERSION 3.16)
project(isp-app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

find_package(Threads REQUIRED)

add_executable(isp-app main.cpp)
target_include_directories(isp-app PRIVATE ${LIBCAMERA_INCLUDE_DIRS})
target_link_libraries(isp-app PRIVATE ${LIBCAMERA_LIBRARIES} Threads::Threads)

install(TARGETS isp-app DESTINATION bin)
