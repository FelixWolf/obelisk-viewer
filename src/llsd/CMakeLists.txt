add_library(viewer_llsd STATIC
  Binary.cpp
  BinaryEncoding.cpp
  Value.cpp
  Xml.cpp
)
add_library(viewer::llsd ALIAS viewer_llsd)

target_include_directories(viewer_llsd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_llsd
  PUBLIC viewer::core
  PRIVATE tinyxml2::tinyxml2 viewer_warnings
)
