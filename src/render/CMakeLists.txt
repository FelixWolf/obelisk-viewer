add_library(viewer_render STATIC
  Renderer.cpp
)
add_library(viewer::render ALIAS viewer_render)

target_include_directories(viewer_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_render
  PUBLIC SDL3::SDL3 viewer::core viewer::image
  PRIVATE viewer_warnings
)
