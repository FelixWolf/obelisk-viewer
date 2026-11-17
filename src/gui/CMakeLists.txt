# The viewer's own interface (RmlUi on SDL GPU): viewer/gui/Gui.hpp.
set(gui_shader_header ${CMAKE_CURRENT_BINARY_DIR}/generated/viewer/gui/GuiShaders.hpp)
viewer_embed_shaders(${gui_shader_header}
  NAMESPACE viewer::gui::shaders
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shaders
  SOURCES gui.vert gui_color.frag gui_texture.frag gui_gradient.frag gui_blit.vert gui_blit.frag
)

add_library(viewer_gui STATIC
  Gui.cpp
  GuiRenderer.cpp
  StbImage.cpp
  ${gui_shader_header}
)
add_library(viewer::gui ALIAS viewer_gui)

target_include_directories(viewer_gui
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(viewer_gui
  PUBLIC SDL3::SDL3 glm::glm RmlUi::RmlUi
  PRIVATE viewer::core viewer::text viewer::stb viewer_warnings
)
