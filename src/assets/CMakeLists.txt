add_library(viewer_assets STATIC
  MeshAsset.cpp
  TextureCache.cpp
)
add_library(viewer::assets ALIAS viewer_assets)

target_include_directories(viewer_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_assets
  PUBLIC viewer::core viewer::image viewer::volume
  PRIVATE viewer::llsd viewer::zlib viewer_warnings
)
