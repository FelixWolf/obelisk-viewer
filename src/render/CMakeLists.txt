# --- Shaders: GLSL -> SPIR-V at build time, embedded as byte arrays ----------
set(shader_sources
  object.vert
  skinned.vert
  blinnphong.frag
  pbr.frag
  terrain.vert
  terrain_blinnphong.frag
  terrain_pbr.frag
  fullscreen.vert
  tonemap.frag
)
set(shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(shader_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(shader_spirv)
set(shader_entries)
foreach(source IN LISTS shader_sources)
  set(spirv ${shader_out}/${source}.spv)
  # object.vert -> kObjectVert, terrain_pbr.frag -> kTerrainPbrFrag
  string(REGEX REPLACE "[._]" ";" parts ${source})
  set(name "k")
  foreach(part IN LISTS parts)
    if(part)
      string(SUBSTRING ${part} 0 1 first)
      string(TOUPPER ${first} first)
      string(SUBSTRING ${part} 1 -1 rest)
      string(APPEND name ${first}${rest})
    endif()
  endforeach()
  add_custom_command(
    OUTPUT ${spirv}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_out}
    COMMAND ${VIEWER_GLSLANG_EXECUTABLE} -V --target-env vulkan1.0 -I${shader_dir}
            -o ${spirv} ${shader_dir}/${source}
    DEPENDS ${shader_dir}/${source} ${shader_dir}/common.glsl ${VIEWER_GLSLANG_DEPENDS}
    COMMENT "Compiling shader ${source}"
    VERBATIM
  )
  list(APPEND shader_spirv ${spirv})
  list(APPEND shader_entries "${name}=${spirv}")
endforeach()

set(shader_header ${CMAKE_CURRENT_BINARY_DIR}/generated/viewer/render/ShaderBytecode.hpp)
add_custom_command(
  OUTPUT ${shader_header}
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${shader_header} -DNAMESPACE=viewer::render::shaders
          "-DENTRIES=${shader_entries}" -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  DEPENDS ${shader_spirv} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
  COMMENT "Embedding SPIR-V shaders"
  VERBATIM
)

add_library(viewer_render STATIC
  Renderer.cpp
  RendererScene.cpp
  ${shader_header}
)
add_library(viewer::render ALIAS viewer_render)

target_include_directories(viewer_render
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(viewer_render
  PUBLIC SDL3::SDL3 glm::glm viewer::core viewer::image
  PRIVATE viewer_warnings
)
