add_executable(viewer_tests
  main.cpp
  CircuitTests.cpp
  HttpClientTests.cpp
  JobSystemTests.cpp
  Ktx2Tests.cpp
  LlsdTests.cpp
  LoginTests.cpp
  Md5Tests.cpp
  ObjectTests.cpp
  ProtocolTests.cpp
  SessionTests.cpp
  TerrainTests.cpp
  TextureCacheTests.cpp
  TextureProcessingTests.cpp
  UuidTests.cpp
  VolumeTests.cpp
)
target_link_libraries(viewer_tests PRIVATE
  doctest::doctest
  viewer::core
  viewer::image
  viewer::assets
  viewer::llsd
  viewer::net
  viewer::protocol
  viewer::session
  viewer::world
  viewer::volume
  viewer_warnings
)
target_compile_definitions(viewer_tests PRIVATE
  VIEWER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  VIEWER_TEST_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
)
add_test(NAME viewer_tests COMMAND viewer_tests)
