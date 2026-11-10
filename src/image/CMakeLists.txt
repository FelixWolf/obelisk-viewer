add_library(viewer_image STATIC
  OpenJpegDecoder.cpp # defines createJ2kDecoder(); swap this file to change backend
)
add_library(viewer::image ALIAS viewer_image)

target_include_directories(viewer_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_image
  PUBLIC viewer::core
  PRIVATE OpenJPEG::openjp2 viewer_warnings
)
