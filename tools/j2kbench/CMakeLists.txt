add_executable(j2kbench
  main.cpp
)
target_link_libraries(j2kbench PRIVATE viewer::image viewer::core viewer_warnings)
