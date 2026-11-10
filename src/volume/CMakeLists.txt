add_library(viewer_volume STATIC
  Volume.cpp
)
add_library(viewer::volume ALIAS viewer_volume)

target_include_directories(viewer_volume PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_volume
  PUBLIC glm::glm
  PRIVATE viewer_warnings
)
