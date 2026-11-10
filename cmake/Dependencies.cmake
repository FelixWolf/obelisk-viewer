# Every third-party dependency, pinned. Keep all CPMAddPackage calls here so
# versions can be audited and bumped in one place.

# --- SDL3 ------------------------------------------------------------------
CPMAddPackage(
  NAME SDL3
  GITHUB_REPOSITORY libsdl-org/SDL
  GIT_TAG release-3.4.20
  OPTIONS
    "SDL_SHARED OFF"
    "SDL_STATIC ON"
    "SDL_TEST_LIBRARY OFF"
    "SDL_EXAMPLES OFF"
    "SDL_TESTS OFF"
    "SDL_INSTALL OFF"
)

# --- Dear ImGui (docking branch) -----------------------------------------
# ImGui ships no CMake build, so we download it and define the target ourselves.
CPMAddPackage(
  NAME imgui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG v1.93.0-docking
  DOWNLOAD_ONLY YES
)
if(imgui_ADDED)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  )
  target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
  )
  target_link_libraries(imgui PUBLIC SDL3::SDL3)
  target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
endif()

# --- OpenJPEG: the viewer's JPEG 2000 decoder (behind viewer::image::J2kDecoder)
CPMAddPackage(
  NAME openjpeg
  GITHUB_REPOSITORY uclouvain/openjpeg
  GIT_TAG v2.5.4
  OPTIONS
    "BUILD_SHARED_LIBS OFF"
    "BUILD_STATIC_LIBS ON"
    "BUILD_CODEC OFF"
    "BUILD_TESTING OFF"
    "BUILD_DOC OFF"
)
if(openjpeg_ADDED)
  # OpenJPEG's in-tree target does not export its include dirs.
  add_library(openjpeg_iface INTERFACE)
  target_link_libraries(openjpeg_iface INTERFACE openjp2)
  target_include_directories(openjpeg_iface SYSTEM INTERFACE
    ${openjpeg_SOURCE_DIR}/src/lib/openjp2
    ${openjpeg_BINARY_DIR}/src/lib/openjp2
  )
  add_library(OpenJPEG::openjp2 ALIAS openjpeg_iface)
endif()

# --- zstd: supercompression for the KTX2 texture cache ----------------------
CPMAddPackage(
  NAME zstd
  GITHUB_REPOSITORY facebook/zstd
  GIT_TAG v1.5.7
  SOURCE_SUBDIR build/cmake
  OPTIONS
    "ZSTD_BUILD_PROGRAMS OFF"
    "ZSTD_BUILD_SHARED OFF"
    "ZSTD_BUILD_STATIC ON"
    "ZSTD_BUILD_TESTS OFF"
    "ZSTD_LEGACY_SUPPORT OFF"
)
if(zstd_ADDED)
  add_library(zstd_iface INTERFACE)
  target_link_libraries(zstd_iface INTERFACE libzstd_static)
  target_include_directories(zstd_iface SYSTEM INTERFACE ${zstd_SOURCE_DIR}/lib)
  add_library(zstd::zstd ALIAS zstd_iface)
endif()

# --- doctest: unit tests ----------------------------------------------------
if(VIEWER_BUILD_TESTS)
  CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG v2.5.3
    OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON"
  )
endif()

# --- tinyxml2: LLSD+XML parsing and writing ---------------------------------
CPMAddPackage(
  NAME tinyxml2
  GITHUB_REPOSITORY leethomason/tinyxml2
  GIT_TAG 11.0.0
  OPTIONS
    "tinyxml2_BUILD_TESTING OFF"
    "BUILD_SHARED_LIBS OFF"
)

# --- libcurl: the first HttpClient backend (behind viewer::net::HttpClient) --
# HTTP(S) only. TLS comes from the platform: Schannel on Windows, OpenSSL elsewhere.
if(WIN32)
  set(VIEWER_CURL_TLS "CURL_USE_SCHANNEL ON")
else()
  set(VIEWER_CURL_TLS "CURL_USE_OPENSSL ON")
endif()
CPMAddPackage(
  NAME curl
  GITHUB_REPOSITORY curl/curl
  GIT_TAG curl-8_23_0
  OPTIONS
    "BUILD_CURL_EXE OFF"
    "BUILD_SHARED_LIBS OFF"
    "BUILD_STATIC_LIBS ON"
    "BUILD_TESTING OFF"
    "BUILD_EXAMPLES OFF"
    "BUILD_LIBCURL_DOCS OFF"
    "BUILD_MISC_DOCS OFF"
    "ENABLE_CURL_MANUAL OFF"
    "CURL_DISABLE_INSTALL ON"
    "CURL_DISABLE_DICT ON"
    "CURL_DISABLE_FILE ON"
    "CURL_DISABLE_FTP ON"
    "CURL_DISABLE_GOPHER ON"
    "CURL_DISABLE_IMAP ON"
    "CURL_DISABLE_IPFS ON"
    "CURL_DISABLE_LDAP ON"
    "CURL_DISABLE_MQTT ON"
    "CURL_DISABLE_POP3 ON"
    "CURL_DISABLE_RTSP ON"
    "CURL_DISABLE_SMTP ON"
    "CURL_DISABLE_TELNET ON"
    "CURL_DISABLE_TFTP ON"
    "CURL_DISABLE_WEBSOCKETS ON"
    "CURL_USE_LIBPSL OFF"
    "CURL_USE_LIBSSH2 OFF"
    "CURL_USE_LIBSSH OFF"
    "USE_LIBIDN2 OFF"
    "USE_NGHTTP2 OFF"
    "CURL_BROTLI OFF"
    "CURL_ZSTD OFF"
    "CURL_ZLIB OFF"
    "${VIEWER_CURL_TLS}"
)
