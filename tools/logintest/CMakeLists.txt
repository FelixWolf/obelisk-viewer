add_executable(logintest
  main.cpp
)
target_link_libraries(logintest PRIVATE viewer::session viewer::world viewer_warnings)
target_compile_definitions(logintest PRIVATE VIEWER_VERSION="${PROJECT_VERSION}")
