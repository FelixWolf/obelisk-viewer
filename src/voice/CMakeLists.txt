add_library(viewer_voice STATIC
  JitterBuffer.cpp
  Messages.cpp
  RtcPeer.cpp
  VoiceClient.cpp
  VoiceConnection.cpp
  VoiceInput.cpp
)
add_library(viewer::voice ALIAS viewer_voice)
target_include_directories(viewer_voice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_voice
  PUBLIC viewer::core viewer::llsd viewer::net viewer::audio glm::glm
  PRIVATE LibDataChannel::LibDataChannelStatic opus viewer::speexdsp viewer_warnings
)
