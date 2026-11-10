add_executable(viewer_tests
  main.cpp
  HttpClientTests.cpp
  JobSystemTests.cpp
  Ktx2Tests.cpp
  LlsdTests.cpp
  TextureCacheTests.cpp
  TextureProcessingTests.cpp
  UuidTests.cpp
)
target_link_libraries(viewer_tests PRIVATE
  doctest::doctest
  viewer::core
  viewer::image
  viewer::assets
  viewer::llsd
  viewer::net
  viewer_warnings
)
target_compile_definitions(viewer_tests PRIVATE
  VIEWER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  VIEWER_TEST_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
)
add_test(NAME viewer_tests COMMAND viewer_tests)
