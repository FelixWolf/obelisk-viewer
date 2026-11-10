add_library(viewer_net STATIC
  HttpClient.cpp
  UdpSocket.cpp
  CurlHttpClient.cpp # defines createHttpClient(); swap this file to change backend
)
add_library(viewer::net ALIAS viewer_net)

target_include_directories(viewer_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(viewer_net
  PUBLIC viewer::core
  PRIVATE CURL::libcurl viewer_warnings
)
if(WIN32)
  target_link_libraries(viewer_net PRIVATE ws2_32)
endif()
