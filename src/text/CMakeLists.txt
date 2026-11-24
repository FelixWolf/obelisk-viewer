# The built-in font (Inter, SIL Open Font License: assets/fonts/LICENSE-Inter.txt),
# embedded in the binary.
set(font_header ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedFonts.hpp)
set(font_regular ${PROJECT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf)
add_custom_command(
  OUTPUT ${font_header}
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${font_header} -DNAMESPACE=viewer::text::embedded
          "-DENTRIES=kInterRegularTtf=${font_regular}" -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  DEPENDS ${font_regular} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  COMMENT "Embedding fonts"
  VERBATIM
)

add_library(viewer_text STATIC
  ${font_header}
  Font.cpp
  GlyphAtlas.cpp
  TextLayout.cpp
)
add_library(viewer::text ALIAS viewer_text)

target_include_directories(viewer_text
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(viewer_text
  PUBLIC glm::glm
  PRIVATE viewer::stb viewer_warnings
)

configure_file(${PROJECT_SOURCE_DIR}/assets/fonts/LICENSE-Inter.txt
               ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LICENSE-Inter.txt COPYONLY)
install(FILES ${PROJECT_SOURCE_DIR}/assets/fonts/LICENSE-Inter.txt DESTINATION . COMPONENT viewer)
