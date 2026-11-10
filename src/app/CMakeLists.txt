add_executable(viewer
  main.cpp
  App.cpp
)

target_link_libraries(viewer PRIVATE
  viewer::core
  viewer::render
  viewer::ui
  viewer_warnings
)

target_compile_definitions(viewer PRIVATE
  $<$<CONFIG:Debug>:VIEWER_GPU_DEBUG=1>
)
