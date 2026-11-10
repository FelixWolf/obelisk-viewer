# Viewer assets embedded in the binary (see assets/LICENSE-artwork.txt).
set(asset_header ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.hpp)
set(asset_ramp ${PROJECT_SOURCE_DIR}/assets/textures/alpha_gradient_2d.j2c)
add_custom_command(
  OUTPUT ${asset_header}
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${asset_header} -DNAMESPACE=viewer::assets_embedded
          "-DENTRIES=kAlphaGradient2dJ2c=${asset_ramp}" -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  DEPENDS ${asset_ramp} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  COMMENT "Embedding viewer assets"
  VERBATIM
)

add_executable(viewer
  ${asset_header}
  main.cpp
  App.cpp
  CameraController.cpp
  LoginPanel.cpp
  Procedural.cpp
  SceneAssets.cpp
  TerrainTextures.cpp
  TerrainView.cpp
  TextureManager.cpp
  TextureBrowser.cpp
)

target_include_directories(viewer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Licenses travel with the binary: ours (LGPL-2.1) and the artwork notice.
configure_file(${PROJECT_SOURCE_DIR}/LICENSE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LICENSE COPYONLY)
configure_file(${PROJECT_SOURCE_DIR}/assets/LICENSE-artwork.txt
               ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LICENSE-artwork.txt COPYONLY)

target_link_libraries(viewer PRIVATE
  viewer::core
  viewer::image
  viewer::assets
  viewer::net
  viewer::session
  viewer::world
  viewer::render
  viewer::ui
  viewer_warnings
)

target_compile_definitions(viewer PRIVATE
  $<$<CONFIG:Debug>:VIEWER_GPU_DEBUG=1>
  VIEWER_VERSION="${PROJECT_VERSION}"
)
