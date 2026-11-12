add_library(viewer_world STATIC
  ObjectCache.cpp
  Objects.cpp
  TerrainDecoder.cpp
  TerrainLayers.cpp
  World.cpp
  WorldThread.cpp
)
add_library(viewer::world ALIAS viewer_world)

target_include_directories(viewer_world PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_world
  PUBLIC viewer::core viewer::protocol viewer::volume
  PRIVATE viewer_warnings
)
