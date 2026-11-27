# --- Shaders: GLSL -> SPIR-V at build time, embedded (cmake/Shaders.cmake) ---
set(shader_sources
  object.vert
  skinned.vert
  blinnphong.frag
  pbr.frag
  terrain.vert
  terrain_blinnphong.frag
  terrain_pbr.frag
  fullscreen.vert
  sky.frag
  shadow.frag
  shadow_masked.frag
  tonemap.frag
  luminance.frag
  exposure.frag
  probe_radiance.frag
  probe_irradiance.frag
  brdf_table.frag
  hud_legacy.frag
  hud_pbr.frag
  hud_composite.frag
  hud_panel.vert
  hud_panel.frag
  pick.frag
  outline_mask.frag
  outline.frag
  label.vert
  label.frag
  panel_image.frag
  glow_mask.frag
  glow_extract.frag
  glow_blur.frag
  occlusion_reduce.frag
  particle.vert
  particle.frag
  particle_glow.frag
  water.vert
  water.frag
  water_mask.frag
  water_mask_object.vert
  eye_copy.frag
  blinnphong_coverage.frag
  pbr_coverage.frag
  depth_resolve.frag
  fxaa.frag
  smaa_edges.vert
  smaa_edges.frag
  smaa_weights.vert
  smaa_weights.frag
  smaa_blend.vert
  smaa_blend.frag
)
set(shader_header ${CMAKE_CURRENT_BINARY_DIR}/generated/viewer/render/ShaderBytecode.hpp)
viewer_embed_shaders(${shader_header}
  NAMESPACE viewer::render::shaders
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shaders
  SOURCES ${shader_sources}
  INCLUDES common.glsl atmosphere.glsl shadow.glsl lights.glsl probes.glsl cubemap.glsl
           tonemap.glsl clusters.glsl coverage.glsl pbr_surface.glsl blinnphong_surface.glsl
           smaa.glsl ${smaa_SOURCE_DIR}/SMAA.hlsl
  INCLUDE_DIRECTORIES ${smaa_SOURCE_DIR}
)

add_library(viewer_render STATIC
  Renderer.cpp
  RendererScene.cpp
  RendererProbes.cpp
  RendererHud.cpp
  RendererPick.cpp
  RendererText.cpp
  RendererGlow.cpp
  RendererParticles.cpp
  RendererWater.cpp
  RendererOcclusion.cpp
  RendererEyes.cpp
  RendererSmaa.cpp
  OcclusionMap.cpp
  Clusters.cpp
  Camera.cpp
  Frustum.cpp
  Projector.cpp
  ShadowCascades.cpp
  MeshPool.cpp
  ${shader_header}
)
add_library(viewer::render ALIAS viewer_render)

target_include_directories(viewer_render
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
)
# SMAA's lookup textures (AreaTex.h, SearchTex.h), for RendererSmaa.cpp.
target_include_directories(viewer_render SYSTEM PRIVATE ${smaa_SOURCE_DIR}/Textures)
target_link_libraries(viewer_render
  PUBLIC SDL3::SDL3 glm::glm viewer::core viewer::image viewer::text
  PRIVATE viewer_warnings
)
