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

# MinGW's libstdc++ keeps std::print's console output (__open_terminal,
# __write_to_terminal) in a separate library.
if(MINGW)
  string(APPEND CMAKE_CXX_STANDARD_LIBRARIES " -lstdc++exp")
endif()

option(VIEWER_BUILD_TOOLS "Build developer tools (j2kbench, ...)" ON)
option(VIEWER_BUILD_TESTS "Build unit tests" ON)

include(cmake/Branding.cmake)
include(cmake/CPM.cmake)
include(cmake/Licenses.cmake)
# Ours first (the About page lists them in this order), then the libraries'
# (cmake/Dependencies.cmake).
viewer_license("${VIEWER_NAME}" "${PROJECT_SOURCE_DIR}/LICENSE")
viewer_license("Inter (the font)" "${PROJECT_SOURCE_DIR}/assets/fonts/LICENSE-Inter.txt")
viewer_license("Second Life viewer artwork" "${PROJECT_SOURCE_DIR}/assets/LICENSE-artwork.txt")
include(cmake/Dependencies.cmake)
include(cmake/Shaders.cmake)

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

if(VIEWER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# A .zip of what `cmake --install --component viewer` puts in place (cpack in
# the build directory). Velopack's installer packs that same folder instead;
# see docs/packaging.md.
set(CPACK_PACKAGE_NAME "${VIEWER_PACKAGE_ID}")
set(CPACK_PACKAGE_VENDOR "${VIEWER_NAME}")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "ZIP")
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
set(CPACK_INSTALL_CMAKE_PROJECTS "${PROJECT_BINARY_DIR};viewer;viewer;/")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/package")
set(CPACK_SOURCE_GENERATOR "")
include(CPack)
