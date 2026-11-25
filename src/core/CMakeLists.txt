add_library(viewer_core STATIC
  FileCache.cpp
  JobSystem.cpp
  Md5.cpp
  ProcessLock.cpp
  Sha256.cpp
  Uuid.cpp
)
add_library(viewer::core ALIAS viewer_core)

# The viewer's names (cmake/Branding.cmake).
configure_file(Branding.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/viewer/core/Branding.hpp @ONLY)

target_include_directories(viewer_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(viewer_core PRIVATE viewer_warnings)
