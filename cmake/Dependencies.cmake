# Every third-party dependency, pinned. Keep all CPMAddPackage calls here so
# versions can be audited and bumped in one place.

# --- SDL3 ------------------------------------------------------------------
CPMAddPackage(
  NAME SDL3
  GITHUB_REPOSITORY libsdl-org/SDL
  # The development branch (3.5), pinned, for its GPU OpenXR support
  # (SDL_openxr.h), which no release has yet; back to a release (3.6) when
  # one does.
  GIT_TAG fda4ece2775c80faf42e548c1631b4dd91b74469
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

# --- GLM: vector/matrix math (header-only) -----------------------------------
CPMAddPackage(
  NAME glm
  GITHUB_REPOSITORY g-truc/glm
  GIT_TAG 1.0.3
  DOWNLOAD_ONLY YES
)
if(glm_ADDED)
  add_library(viewer_glm INTERFACE)
  target_include_directories(viewer_glm SYSTEM INTERFACE ${glm_SOURCE_DIR})
  # Right-handed, depth 0..1 (SDL GPU's convention on every backend).
  target_compile_definitions(viewer_glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
  add_library(glm::glm ALIAS viewer_glm)
endif()

# --- glslang: compiles our GLSL shaders to SPIR-V at build time ---------------
# Uses an installed glslangValidator / glslang when there is one; otherwise
# builds glslang's command-line compiler from source.
option(VIEWER_BUNDLED_GLSLANG "Always build glslang instead of using an installed one" OFF)
if(NOT VIEWER_BUNDLED_GLSLANG)
  find_program(VIEWER_GLSLANG_EXECUTABLE NAMES glslangValidator glslang)
endif()
if(VIEWER_BUNDLED_GLSLANG OR NOT VIEWER_GLSLANG_EXECUTABLE)
  CPMAddPackage(
    NAME glslang
    GITHUB_REPOSITORY KhronosGroup/glslang
    GIT_TAG 16.6.0
    OPTIONS
      "ENABLE_OPT OFF"
      "ENABLE_HLSL OFF"
      "ENABLE_SPVREMAPPER OFF"
      "ENABLE_GLSLANG_BINARIES ON"
      "GLSLANG_TESTS OFF"
      "GLSLANG_ENABLE_INSTALL OFF"
      "BUILD_EXTERNAL OFF"
      "BUILD_SHARED_LIBS OFF"
  )
  set(VIEWER_GLSLANG_EXECUTABLE $<TARGET_FILE:glslang-standalone>)
  set(VIEWER_GLSLANG_DEPENDS glslang-standalone)
endif()
message(STATUS "Shader compiler: ${VIEWER_GLSLANG_EXECUTABLE}")

# --- zlib: inflating mesh assets (zlib-compressed LLSD) ----------------------
CPMAddPackage(
  NAME zlib
  GITHUB_REPOSITORY madler/zlib
  GIT_TAG v1.3.2
  DOWNLOAD_ONLY YES
)
if(zlib_ADDED)
  # Built from its sources directly, so its CMake options don't matter.
  add_library(viewer_zlib STATIC
    ${zlib_SOURCE_DIR}/adler32.c ${zlib_SOURCE_DIR}/compress.c ${zlib_SOURCE_DIR}/crc32.c
    ${zlib_SOURCE_DIR}/deflate.c ${zlib_SOURCE_DIR}/infback.c ${zlib_SOURCE_DIR}/inffast.c
    ${zlib_SOURCE_DIR}/inflate.c ${zlib_SOURCE_DIR}/inftrees.c ${zlib_SOURCE_DIR}/trees.c
    ${zlib_SOURCE_DIR}/uncompr.c ${zlib_SOURCE_DIR}/zutil.c
  )
  target_include_directories(viewer_zlib SYSTEM PUBLIC ${zlib_SOURCE_DIR})
  set_target_properties(viewer_zlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(viewer::zlib ALIAS viewer_zlib)
endif()

# --- stb_truetype: font loading and glyph rasterizing (viewer::text) ---------
CPMAddPackage(
  NAME stb
  GITHUB_REPOSITORY nothings/stb
  GIT_TAG 9600522de53973336fa033a289858a4354cbb04c
  DOWNLOAD_ONLY YES
)
if(stb_ADDED)
  add_library(viewer_stb INTERFACE)
  target_include_directories(viewer_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
  add_library(viewer::stb ALIAS viewer_stb)
endif()

# --- keychain: the system credential store (saved logins) -------------------
# Secret Service through libsecret on Linux (GNOME Keyring, KWallet), the
# Keychain on macOS, the Credential Manager on Windows.
CPMAddPackage(
  NAME keychain
  GITHUB_REPOSITORY hrantzsch/keychain
  GIT_TAG v1.3.1
  OPTIONS "BUILD_TESTS OFF"
)

# --- FreeType: RmlUi's font engine rasterizes with it -----------------------
CPMAddPackage(
  NAME freetype
  GITHUB_REPOSITORY freetype/freetype
  GIT_TAG VER-2-13-3
  OPTIONS
    "FT_DISABLE_ZLIB ON"
    "FT_DISABLE_BZIP2 ON"
    "FT_DISABLE_PNG ON"
    "FT_DISABLE_HARFBUZZ ON"
    "FT_DISABLE_BROTLI ON"
    "BUILD_SHARED_LIBS OFF"
    "SKIP_INSTALL_ALL ON"
)

# --- RmlUi: the viewer's own interface (HTML/CSS-like documents) -------------
# Its font engine takes FreeType as the target above (not an installed one).
if(TARGET freetype AND NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS freetype)
endif()
CPMAddPackage(
  NAME RmlUi
  GITHUB_REPOSITORY mikke89/RmlUi
  GIT_TAG 6.1
  SYSTEM YES # its headers aren't held to our warnings
  OPTIONS
    "BUILD_SHARED_LIBS OFF"
    "RMLUI_FONT_ENGINE freetype"
    "RMLUI_SAMPLES OFF"
    "RMLUI_PRECOMPILED_HEADERS OFF"
    # Its bundled hash map (robin_hood.h) lacks <cstdint> for current GCC.
    "RMLUI_THIRDPARTY_CONTAINERS OFF"
    "CMAKE_DISABLE_FIND_PACKAGE_Freetype ON"
)
