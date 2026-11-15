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
  FrameProfile.cpp
  AnimationManager.cpp
  AutomaticProbes.cpp
  AvatarNames.cpp
  AvatarView.cpp
  AgentController.cpp
  CameraController.cpp
  EnvironmentManager.cpp
  FaceUpload.cpp
  FlexibleView.cpp
  FloatingText.cpp
  LocalChat.cpp
  LoginPanel.cpp
  LegacyMaterialManager.cpp
  MaterialManager.cpp
  MeshManager.cpp
  NameTags.cpp
  ObjectMotion.cpp
  ObjectIndex.cpp
  ObjectProperties.cpp
  ObjectView.cpp
  ParticleView.cpp
  PointerController.cpp
  Procedural.cpp
  SceneAssets.cpp
  TerrainTextures.cpp
  TerrainOverrides.cpp
  ViewerEffects.cpp
  TerrainView.cpp
  TextureManager.cpp
  TreeView.cpp
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
  viewer::environment
  viewer::avatar
  viewer::volume
  viewer::render
  viewer::ui
  viewer::xr
  viewer_warnings
)

target_compile_definitions(viewer PRIVATE
  $<$<CONFIG:Debug>:VIEWER_GPU_DEBUG=1>
  VIEWER_VERSION="${PROJECT_VERSION}"
)

# Data files the viewer loads at runtime (the default avatar), next to the
# binary.
add_custom_command(TARGET viewer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
          ${PROJECT_SOURCE_DIR}/assets/character
          $<TARGET_FILE_DIR:viewer>/assets/character
  VERBATIM
)
