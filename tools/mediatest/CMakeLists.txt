add_executable(mediatest main.cpp)
target_link_libraries(mediatest PRIVATE viewer::media viewer::image SDL3::SDL3 viewer_warnings)
