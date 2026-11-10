add_library(viewer_ui STATIC
  ImGuiLayer.cpp
)
add_library(viewer::ui ALIAS viewer_ui)

target_include_directories(viewer_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_ui
  PUBLIC imgui viewer::render
  PRIVATE viewer_warnings
)
