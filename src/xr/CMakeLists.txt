# VR headsets through OpenXR (SDL's GPU OpenXR support). OpenXR's headers
# are the copy SDL builds with; SDL loads the OpenXR loader at run time.
add_library(viewer_xr STATIC
  Headset.cpp
  SteamVr.cpp
)
add_library(viewer::xr ALIAS viewer_xr)

target_include_directories(viewer_xr
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(viewer_xr SYSTEM
  PRIVATE ${SDL3_SOURCE_DIR}/src/video/khronos
)
target_link_libraries(viewer_xr
  PUBLIC SDL3::SDL3 glm::glm viewer::core
  PRIVATE viewer_warnings
)
