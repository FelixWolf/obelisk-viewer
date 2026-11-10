add_library(viewer_core INTERFACE)
add_library(viewer::core ALIAS viewer_core)

target_include_directories(viewer_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_core INTERFACE viewer_warnings)
