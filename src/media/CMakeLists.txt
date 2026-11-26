# The viewer's end of media on prims: what it says to the media host, the
# shared memory frames come in, the host program started and listened to.
# No CEF here: only the host (host/) links it.
# What both ends share (the host links only this).
add_library(viewer_media_ipc STATIC
  Protocol.cpp
  SharedMemory.cpp
  UrlPolicy.cpp
)
target_include_directories(viewer_media_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_media_ipc PRIVATE viewer_warnings)
if(UNIX AND NOT APPLE)
  target_link_libraries(viewer_media_ipc PUBLIC rt)
endif()

add_library(viewer_media STATIC
  ChildProcess.cpp
  MediaHost.cpp
)
add_library(viewer::media ALIAS viewer_media)
target_link_libraries(viewer_media PUBLIC viewer_media_ipc viewer::core viewer::audio
                      PRIVATE viewer_warnings)

if(VIEWER_MEDIA)
  add_subdirectory(host)
endif()
