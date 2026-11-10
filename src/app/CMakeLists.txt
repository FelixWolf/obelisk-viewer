add_executable(viewer
  main.cpp
  App.cpp
  LoginPanel.cpp
  TextureBrowser.cpp
)

target_link_libraries(viewer PRIVATE
  viewer::core
  viewer::image
  viewer::assets
  viewer::net
  viewer::session
  viewer::render
  viewer::ui
  viewer_warnings
)

target_compile_definitions(viewer PRIVATE
  $<$<CONFIG:Debug>:VIEWER_GPU_DEBUG=1>
  VIEWER_VERSION="${PROJECT_VERSION}"
)
