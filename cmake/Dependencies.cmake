# Every third-party dependency, pinned. Keep all CPMAddPackage calls here so
# versions can be audited and bumped in one place.

# --- SDL3 ------------------------------------------------------------------
CPMAddPackage(
  NAME SDL3
  GITHUB_REPOSITORY libsdl-org/SDL
  GIT_TAG release-3.4.20
  OPTIONS
    "SDL_SHARED OFF"
    "SDL_STATIC ON"
    "SDL_TEST_LIBRARY OFF"
    "SDL_EXAMPLES OFF"
    "SDL_TESTS OFF"
    "SDL_INSTALL OFF"
)

# --- Dear ImGui (docking branch) -----------------------------------------
# ImGui ships no CMake build, so we download it and define the target ourselves.
CPMAddPackage(
  NAME imgui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG v1.93.0-docking
  DOWNLOAD_ONLY YES
)
if(imgui_ADDED)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
  )
  target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_link_libraries(imgui PUBLIC SDL3::SDL3)
  target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
endif()

# --- OpenJPEG: the viewer's JPEG 2000 decoder (behind viewer::image::J2kDecoder)
CPMAddPackage(
  NAME openjpeg
  GITHUB_REPOSITORY uclouvain/openjpeg
  GIT_TAG v2.5.4
  OPTIONS
    "BUILD_SHARED_LIBS OFF"
    "BUILD_STATIC_LIBS ON"
    "BUILD_CODEC OFF"
    "BUILD_TESTING OFF"
    "BUILD_DOC OFF"
)
if(openjpeg_ADDED)
  # OpenJPEG's in-tree target does not export its include dirs.
  add_library(openjpeg_iface INTERFACE)
  target_link_libraries(openjpeg_iface INTERFACE openjp2)
  target_include_directories(openjpeg_iface SYSTEM INTERFACE
    ${openjpeg_SOURCE_DIR}/src/lib/openjp2
    ${openjpeg_BINARY_DIR}/src/lib/openjp2
  )
  add_library(OpenJPEG::openjp2 ALIAS openjpeg_iface)
endif()
