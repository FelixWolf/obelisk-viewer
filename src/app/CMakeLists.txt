# Viewer assets embedded in the binary (see assets/LICENSE-artwork.txt).
set(asset_header ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.hpp)
set(asset_ramp ${PROJECT_SOURCE_DIR}/assets/textures/alpha_gradient_2d.j2c)
set(asset_cloud ${PROJECT_SOURCE_DIR}/assets/textures/cloud-particle.j2c)
# Every license of what's in the viewer, in one text (cmake/Licenses.cmake).
set(asset_licenses ${CMAKE_CURRENT_BINARY_DIR}/generated/licenses.txt)
viewer_write_licenses(${asset_licenses})
add_custom_command(
  OUTPUT ${asset_header}
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${asset_header} -DNAMESPACE=viewer::assets_embedded
          "-DENTRIES=kAlphaGradient2dJ2c=${asset_ramp};kCloudParticleJ2c=${asset_cloud};kLicensesTxt=${asset_licenses};kIconPng=${VIEWER_ICON}.png"
          -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  DEPENDS ${asset_ramp} ${asset_cloud} ${asset_licenses} ${VIEWER_ICON}.png ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
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
  AvatarLoading.cpp
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
  TermsPage.cpp
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
  MediaKeys.cpp
  MediaManager.cpp
  MediaWhitelist.cpp
  MicrophoneList.cpp
  VoiceIndicators.cpp
  OutfitAttachments.cpp
  Destinations.cpp
  LobbyBoards.cpp
  ScriptDialogPage.cpp
  ScriptDialogs.cpp
  Updater.cpp
  ViewerConfig.cpp
  GridPopulation.cpp
  InstantMessages.cpp
  Offers.cpp
  InventoryBrowser.cpp
  ChatView.cpp
  SoundAssets.cpp
  SoundManager.cpp
  People.cpp
  PeopleView.cpp
  Settings.cpp
  SettingsView.cpp
  ViewerSettings.cpp
  CurrentOutfit.cpp
  ParcelInfo.cpp
  ParcelSounds.cpp
  MusicStream.cpp
  ObjectMedia.cpp
  MainMenu.cpp
  RadialMenu.cpp
  VrControls.cpp
  VrKeyboard.cpp
  TerrainView.cpp
  TextureManager.cpp
  TreeView.cpp
  TextureBrowser.cpp
)

# The program's own name (cmake/Branding.cmake); the target's stays.
set_target_properties(viewer PROPERTIES OUTPUT_NAME "${VIEWER_EXECUTABLE}")

# Windows: the icon, in the program itself. Elsewhere App.cpp gives the window
# the embedded .png.
if(WIN32)
  enable_language(RC)
  configure_file(viewer.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/viewer.rc @ONLY)
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/generated/viewer.rc
    PROPERTIES OBJECT_DEPENDS ${VIEWER_ICON}.ico)
  target_sources(viewer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/viewer.rc)
endif()

# Windows: a console window beside the viewer (its log) only in Debug builds;
# the others are GUI programs (WinMain from SDL_main.h). No effect elsewhere.
set_target_properties(viewer PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)

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
  viewer::audio
  viewer::voice
  viewer::volume
  viewer::render
  viewer::ui
  viewer::gui
  viewer::xr
  viewer::media
  viewer_warnings
)

if(TARGET keychain)
  target_link_libraries(viewer PRIVATE keychain)
  target_compile_definitions(viewer PRIVATE VIEWER_HAVE_KEYCHAIN=1)
endif()

# Windows: the OpenXR loader SDL opens for a headset, beside the viewer (see
# cmake/Dependencies.cmake).
if(WIN32)
  add_custom_command(TARGET viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VIEWER_OPENXR_LOADER_DLL}" $<TARGET_FILE_DIR:viewer>
    VERBATIM)
  install(FILES "${VIEWER_OPENXR_LOADER_DLL}" DESTINATION . COMPONENT viewer)
endif()

# Packaged builds (./dist.sh): main() runs Velopack's install/update hooks
# first, and its DLL (Windows) goes beside the viewer.
if(VIEWER_VELOPACK)
  target_link_libraries(viewer PRIVATE velopack)
  target_compile_definitions(viewer PRIVATE VIEWER_VELOPACK=1)
  if(WIN32)
    add_custom_command(TARGET viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:velopack>
              $<TARGET_FILE_DIR:viewer>/${VIEWER_VELOPACK_DLL_NAME}
      VERBATIM)
    install(FILES $<TARGET_FILE:velopack> DESTINATION . RENAME ${VIEWER_VELOPACK_DLL_NAME}
            COMPONENT viewer)
  endif()
endif()

# VIEWER_TEST_OPTIONS: the command line's testing and measuring options
# (--login-file, --screenshot, --stereo-benchmark, ...; App.hpp's
# TestOptions) in Debug builds, and in others configured with
# -DVIEWER_TEST_OPTIONS=ON (to measure an optimized build).
# Built with the media host: its settings show.
if(VIEWER_MEDIA)
  target_compile_definitions(viewer PRIVATE VIEWER_HAVE_MEDIA=1)
  add_dependencies(viewer viewer_media_host)
endif()

target_compile_definitions(viewer PRIVATE
  $<$<CONFIG:Debug>:VIEWER_GPU_DEBUG=1>
  $<$<OR:$<CONFIG:Debug>,$<BOOL:${VIEWER_TEST_OPTIONS}>>:VIEWER_TEST_OPTIONS=1>
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

# What ships (cmake --install BUILD --component viewer; the "dist" preset):
# the viewer, its runtime data and the licenses, laid out as in bin/ (it
# finds assets/ beside itself). Only the "viewer" component: the
# dependencies' own install rules (static libraries, headers) and the
# developer tools and tests stay out.
install(TARGETS viewer RUNTIME DESTINATION . COMPONENT viewer)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/assets/character ${PROJECT_SOURCE_DIR}/assets/ui
        DESTINATION assets COMPONENT viewer)
if(EXISTS ${PROJECT_SOURCE_DIR}/default_scene/start.scene)
  install(FILES ${PROJECT_SOURCE_DIR}/default_scene/start.scene
          DESTINATION assets RENAME home.scene COMPONENT viewer)
endif()
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/assets/LICENSE-artwork.txt
        DESTINATION . COMPONENT viewer)
