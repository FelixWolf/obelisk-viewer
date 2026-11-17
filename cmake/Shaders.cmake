# GLSL -> SPIR-V at build time, embedded as byte arrays in a header.
#
# viewer_embed_shaders(<header> NAMESPACE <ns> DIRECTORY <dir>
#                      SOURCES <file>... [INCLUDES <file>...])
# Compiles each source in <dir> (object.vert -> kObjectVert, terrain_pbr.frag
# -> kTerrainPbrFrag) and writes <header> declaring them in <ns>. INCLUDES
# are files in <dir> the sources include: changing one recompiles them all.
function(viewer_embed_shaders header)
  cmake_parse_arguments(ARG "" "NAMESPACE;DIRECTORY" "SOURCES;INCLUDES" ${ARGN})
  get_filename_component(out ${header} DIRECTORY)
  set(out ${out}/spirv)
  set(includes)
  foreach(include IN LISTS ARG_INCLUDES)
    list(APPEND includes ${ARG_DIRECTORY}/${include})
  endforeach()
  set(spirvs)
  set(entries)
  foreach(source IN LISTS ARG_SOURCES)
    set(spirv ${out}/${source}.spv)
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
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out}
      COMMAND ${VIEWER_GLSLANG_EXECUTABLE} -V --target-env vulkan1.0 -I${ARG_DIRECTORY}
              -o ${spirv} ${ARG_DIRECTORY}/${source}
      DEPENDS ${ARG_DIRECTORY}/${source} ${includes} ${VIEWER_GLSLANG_DEPENDS}
      COMMENT "Compiling shader ${source}"
      VERBATIM
    )
    list(APPEND spirvs ${spirv})
    list(APPEND entries "${name}=${spirv}")
  endforeach()
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${header} -DNAMESPACE=${ARG_NAMESPACE}
            "-DENTRIES=${entries}" -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    DEPENDS ${spirvs} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    COMMENT "Embedding SPIR-V shaders"
    VERBATIM
  )
endfunction()
