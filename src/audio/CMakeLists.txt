# The third-party implementations, without our warnings.
add_library(viewer_audio_thirdparty STATIC
  MiniaudioImpl.c
  StbVorbisImpl.c
)
target_link_libraries(viewer_audio_thirdparty PUBLIC viewer::miniaudio viewer::stb)
target_compile_definitions(viewer_audio_thirdparty PUBLIC
  MA_NO_ENCODING
  MA_NO_FLAC
  MA_NO_WAV
  MA_NO_RESOURCE_MANAGER
)
set_target_properties(viewer_audio_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(viewer_audio_thirdparty PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

add_library(viewer_audio STATIC
  AudioEngine.cpp
  SoundClip.cpp
)
add_library(viewer::audio ALIAS viewer_audio)
target_include_directories(viewer_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_audio
  PUBLIC viewer::core glm::glm
  PRIVATE viewer_audio_thirdparty viewer_warnings
)
