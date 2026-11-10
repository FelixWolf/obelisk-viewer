add_library(viewer_assets STATIC
  TextureCache.cpp
)
add_library(viewer::assets ALIAS viewer_assets)

target_include_directories(viewer_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_assets
  PUBLIC viewer::core viewer::image
  PRIVATE viewer_warnings
)
