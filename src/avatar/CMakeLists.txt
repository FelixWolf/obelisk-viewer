add_library(viewer_avatar STATIC
  BaseAvatar.cpp
  Shape.cpp
)
add_library(viewer::avatar ALIAS viewer_avatar)

target_include_directories(viewer_avatar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_avatar
  PUBLIC glm::glm
  PRIVATE tinyxml2::tinyxml2 viewer_warnings
)
