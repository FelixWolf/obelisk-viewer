add_library(viewer_environment STATIC
  DayCycle.cpp
  Sky.cpp
)
add_library(viewer::environment ALIAS viewer_environment)

target_include_directories(viewer_environment PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_environment
  PUBLIC viewer::core viewer::llsd glm::glm
  PRIVATE viewer_warnings
)
