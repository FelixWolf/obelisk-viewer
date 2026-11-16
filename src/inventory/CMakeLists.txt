# Inventory: the model (folders and items) and the AISv3 client that fills it.
add_library(viewer_inventory STATIC
  Model.cpp
  Ais.cpp
)
add_library(viewer::inventory ALIAS viewer_inventory)

target_include_directories(viewer_inventory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_inventory
  PUBLIC viewer::core viewer::llsd viewer::net
  PRIVATE viewer_warnings
)
