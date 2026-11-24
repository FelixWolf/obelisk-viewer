# Standalone build of the message code generator for the build machine, used
# when cross-compiling (src/protocol/CMakeLists.txt builds it with the host
# compiler through ExternalProject). Native builds compile main.cpp directly.
cmake_minimum_required(VERSION 3.25)
project(viewer_msggen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(viewer_msggen main.cpp)
