add_executable(httpget
  main.cpp
)
target_link_libraries(httpget PRIVATE viewer::net viewer::llsd viewer_warnings)
