cmake_minimum_required(VERSION 3.25)

project(viewer
  VERSION 0.1.0
  DESCRIPTION "A multithreaded Second Life viewer"
  LANGUAGES C CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset debug")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VIEWER_BUILD_TOOLS "Build developer tools (j2kbench, ...)" ON)

include(cmake/CPM.cmake)
include(cmake/Dependencies.cmake)

# Put our binaries in build/<preset>/bin regardless of what dependencies set.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

# Warning flags for our own code only (never applied to dependencies).
add_library(viewer_warnings INTERFACE)
if(MSVC)
  target_compile_options(viewer_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(viewer_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

add_subdirectory(src)

if(VIEWER_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
