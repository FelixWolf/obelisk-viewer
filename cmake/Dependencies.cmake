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

# --- libogg and libvorbis: decoding sounds (viewer::audio) -------------------
# The reference decoder: Second Life's oldest sounds use Vorbis features
# (floor 0) lighter decoders leave out.
CPMAddPackage(
  NAME ogg
  GITHUB_REPOSITORY xiph/ogg
  GIT_TAG v1.3.5
  OPTIONS
    # Asks for an old CMake that current ones refuse otherwise.
    "CMAKE_POLICY_VERSION_MINIMUM 3.5"
    "BUILD_SHARED_LIBS OFF"
    "INSTALL_DOCS OFF"
    "BUILD_TESTING OFF"
)
CPMAddPackage(
  NAME vorbis
  GITHUB_REPOSITORY xiph/vorbis
  GIT_TAG v1.3.7
  OPTIONS
    "CMAKE_POLICY_VERSION_MINIMUM 3.5"
    "BUILD_SHARED_LIBS OFF"
    "BUILD_TESTING OFF"
)

# --- miniaudio: sound output, mixing and 3D positioning (viewer::audio) -----
# One header, compiled once in src/audio/MiniaudioImpl.c; its own decoders
# aren't used (Ogg Vorbis goes through stb_vorbis).
CPMAddPackage(
  NAME miniaudio
  GITHUB_REPOSITORY mackron/miniaudio
  GIT_TAG 0.11.22
  DOWNLOAD_ONLY YES
)
if(miniaudio_ADDED)
  add_library(viewer_miniaudio INTERFACE)
  target_include_directories(viewer_miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
  add_library(viewer::miniaudio ALIAS viewer_miniaudio)
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

# --- LunaSVG: RmlUi's SVG plugin draws <svg> images with it (icons) ---------
# 2.x: its rasterizer (plutovg) is bundled; RmlUi 6.1 uses its 2.x API.
CPMAddPackage(
  NAME lunasvg
  GITHUB_REPOSITORY sammycage/lunasvg
  GIT_TAG v2.4.1
  SYSTEM YES
  OPTIONS "BUILD_SHARED_LIBS OFF" "LUNASVG_BUILD_EXAMPLES OFF"
)
if(TARGET lunasvg AND NOT TARGET lunasvg::lunasvg)
  add_library(lunasvg::lunasvg ALIAS lunasvg)
endif()
# lunasvg defines this only for itself, so on Windows its users (RmlUi's SVG
# plugin) would otherwise declare its functions dllimport.
if(TARGET lunasvg AND NOT BUILD_SHARED_LIBS)
  target_compile_definitions(lunasvg INTERFACE LUNASVG_BUILD_STATIC)
endif()

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
    # SVG images, with the LunaSVG above (not an installed one).
    "RMLUI_SVG_PLUGIN ON"
    "CMAKE_DISABLE_FIND_PACKAGE_lunasvg ON"
)

# --- OpenXR loader (Windows): what SDL opens for VR --------------------------
# SDL loads openxr_loader.dll at run time (no linking). Linux has it from the
# system's OpenXR package; Windows has none (VR runtimes don't install one),
# so the Khronos loader ships beside the viewer. Prebuilt (MSVC, but only
# loaded through its C interface; it needs only system DLLs).
if(WIN32)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "OpenXR loader: only x64 Windows set up")
  endif()
  CPMAddPackage(
    NAME openxr_loader
    URL https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.65/openxr_loader_windows-1.1.65.zip
    URL_HASH SHA256=859d7de35b7a968112db770b09ed8bddeed3cb16fd8ae070643a857676bea6fd
    DOWNLOAD_ONLY YES
  )
  set(VIEWER_OPENXR_LOADER_DLL "${openxr_loader_SOURCE_DIR}/x64/bin/openxr_loader.dll"
      CACHE INTERNAL "")
endif()

# --- Velopack: installer and updater hooks (./dist.sh) -----------------------
# Off for everyday builds; ./dist.sh turns it on. Its prebuilt C library:
# a DLL on Windows (MSVC-built, but C-only, so MinGW links it; it needs only
# system DLLs), a static library on Linux.
option(VIEWER_VELOPACK "Link Velopack's app hooks (packaged builds; ./dist.sh)" OFF)
if(VIEWER_VELOPACK)
  set(VIEWER_VELOPACK_VERSION 1.2.161)
  CPMAddPackage(
    NAME velopack
    URL https://github.com/velopack/velopack/releases/download/${VIEWER_VELOPACK_VERSION}/velopack_libc_${VIEWER_VELOPACK_VERSION}.zip
    URL_HASH SHA256=31de8f2cf05a47bd5328a9f98b0c41aec1582156f6075f1058b26941f7f17c20
    DOWNLOAD_ONLY YES
  )
  if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    add_library(velopack SHARED IMPORTED GLOBAL)
    set_target_properties(velopack PROPERTIES
      IMPORTED_LOCATION "${velopack_SOURCE_DIR}/lib/velopack_libc_win_x64_msvc.dll"
      IMPORTED_IMPLIB "${velopack_SOURCE_DIR}/lib/velopack_libc_win_x64_msvc.dll.lib")
    # The name its import library links against (and so what has to be
    # beside the viewer), not the file's in the zip.
    set(VIEWER_VELOPACK_DLL_NAME velopack_libc.dll CACHE INTERNAL "")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    add_library(velopack STATIC IMPORTED GLOBAL)
    set_target_properties(velopack PROPERTIES
      IMPORTED_LOCATION "${velopack_SOURCE_DIR}/lib-static/velopack_libc_linux_x64_gnu.a")
    find_package(Threads REQUIRED)
    target_link_libraries(velopack INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
  else()
    message(FATAL_ERROR "VIEWER_VELOPACK: no Velopack library for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR} here")
  endif()
  target_include_directories(velopack INTERFACE "${velopack_SOURCE_DIR}/include")
  # The zip has no license; it is the repository's (MIT), at the same tag.
  set(velopack_license "${CMAKE_BINARY_DIR}/velopack-LICENSE")
  file(DOWNLOAD
    https://raw.githubusercontent.com/velopack/velopack/${VIEWER_VELOPACK_VERSION}/LICENSE
    "${velopack_license}"
    EXPECTED_HASH SHA256=91845db83551c877ebbb1118e0fb92e4e527290d23b995c55dcd438b3293943f)
endif()

# --- The licenses of the libraries above that ship in the viewer ------------
# (cmake/Licenses.cmake; the About page lists them). Not glslang (a build
# tool) or doctest (the tests').
viewer_license("SDL" "${SDL3_SOURCE_DIR}/LICENSE.txt")
viewer_license("Dear ImGui" "${imgui_SOURCE_DIR}/LICENSE.txt")
viewer_license("RmlUi" "${RmlUi_SOURCE_DIR}/LICENSE.txt")
viewer_license("LunaSVG" "${lunasvg_SOURCE_DIR}/LICENSE")
# Dual-licensed: we take the FreeType License.
viewer_license("FreeType" "${freetype_SOURCE_DIR}/docs/FTL.TXT")
viewer_license("OpenJPEG" "${openjpeg_SOURCE_DIR}/LICENSE")
# Dual-licensed (BSD or GPLv2): we take the BSD license.
viewer_license("Zstandard" "${zstd_SOURCE_DIR}/LICENSE")
viewer_license("zlib" "${zlib_SOURCE_DIR}/LICENSE")
viewer_license("curl" "${curl_SOURCE_DIR}/COPYING")
viewer_license("TinyXML-2" "${tinyxml2_SOURCE_DIR}/LICENSE.txt")
viewer_license("GLM" "${glm_SOURCE_DIR}/copying.txt")
viewer_license("stb" "${stb_SOURCE_DIR}/LICENSE")
viewer_license("miniaudio" "${miniaudio_SOURCE_DIR}/LICENSE")
viewer_license("libogg" "${ogg_SOURCE_DIR}/COPYING")
viewer_license("libvorbis" "${vorbis_SOURCE_DIR}/COPYING")
viewer_license("keychain" "${keychain_SOURCE_DIR}/LICENSE")
if(WIN32)
  viewer_license("OpenXR loader" "${openxr_loader_SOURCE_DIR}/share/doc/openxr/LICENSE")
endif()
if(VIEWER_VELOPACK)
  viewer_license("Velopack" "${velopack_license}")
endif()
