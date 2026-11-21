# Viewer assets embedded in the binary (see assets/LICENSE-artwork.txt).
set(asset_header ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.hpp)
set(asset_ramp ${PROJECT_SOURCE_DIR}/assets/textures/alpha_gradient_2d.j2c)
# Every license of what's in the viewer, in one text (cmake/Licenses.cmake).
set(asset_licenses ${CMAKE_CURRENT_BINARY_DIR}/generated/licenses.txt)
viewer_write_licenses(${asset_licenses})
add_custom_command(
  OUTPUT ${asset_header}
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${asset_header} -DNAMESPACE=viewer::assets_embedded
          "-DENTRIES=kAlphaGradient2dJ2c=${asset_ramp};kLicensesTxt=${asset_licenses}"
          -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  DEPENDS ${asset_ramp} ${asset_licenses} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
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
  AccountPicture.cpp
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
  LoginScreen.cpp
  LoginFlow.cpp
  SavedLogins.cpp
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
  OutfitAttachments.cpp
  Destinations.cpp
  LobbyBoards.cpp
  ScriptDialogPage.cpp
  ScriptDialogs.cpp
  ViewerConfig.cpp
  GridPopulation.cpp
  InventoryBrowser.cpp
  ChatView.cpp
  CurrentOutfit.cpp
  ParcelInfo.cpp
  MainMenu.cpp
  RadialMenu.cpp
  VrControls.cpp
  VrKeyboard.cpp
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
  viewer::inventory
  viewer::world
  viewer::environment
  viewer::avatar
  viewer::volume
  viewer::render
  viewer::ui
  viewer::gui
  viewer::xr
  viewer_warnings
)

if(TARGET keychain)
  target_link_libraries(viewer PRIVATE keychain)
  target_compile_definitions(viewer PRIVATE VIEWER_HAVE_KEYCHAIN=1)
endif()

target_compile_definitions(viewer PRIVATE
  $<$<CONFIG:Debug>:VIEWER_GPU_DEBUG=1>
  VIEWER_VERSION="${PROJECT_VERSION}"
)

# Data files the viewer loads at runtime (the default avatar, the
# interface's documents, the lobby's scene), next to the binary. Copied by a
# target of their own that runs on every build, so a change to only them
# (an .rml, an .rcss) gets there too (a POST_BUILD step runs only when the
# viewer is linked again).
set(VIEWER_COPY_ASSETS
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
          ${PROJECT_SOURCE_DIR}/assets/character
          ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/character
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
          ${PROJECT_SOURCE_DIR}/assets/ui
          ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/ui
)
# The lobby's scene (made by default_scene/sceneConvert.py), if there is one.
if(EXISTS ${PROJECT_SOURCE_DIR}/default_scene/start.scene)
  list(APPEND VIEWER_COPY_ASSETS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/default_scene/start.scene
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/home.scene
  )
endif()
add_custom_target(viewer_data ALL ${VIEWER_COPY_ASSETS} VERBATIM)
add_dependencies(viewer viewer_data)
