add_library(viewer_core STATIC
  JobSystem.cpp
  Md5.cpp
  Uuid.cpp
)
add_library(viewer::core ALIAS viewer_core)

target_include_directories(viewer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_core PRIVATE viewer_warnings)
