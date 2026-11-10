add_library(viewer_session STATIC
  Credentials.cpp
  Login.cpp
  MachineInfo.cpp
  Session.cpp
)
add_library(viewer::session ALIAS viewer_session)

target_include_directories(viewer_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_session
  PUBLIC viewer::core viewer::llsd viewer::net viewer::protocol
  PRIVATE viewer_warnings
)
if(WIN32)
  target_link_libraries(viewer_session PRIVATE iphlpapi)
endif()
