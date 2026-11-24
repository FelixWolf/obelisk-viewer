# Cross-compiles for 64-bit Windows from Linux with MinGW-w64 (GCC).
# Used by the windows-* presets; `./build.sh windows release`.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(VIEWER_MINGW_TRIPLE x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${VIEWER_MINGW_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${VIEWER_MINGW_TRIPLE}-g++)
set(CMAKE_RC_COMPILER ${VIEWER_MINGW_TRIPLE}-windres)

# Libraries and headers come from the MinGW sysroot; programs (glslang) from
# the host, since they run during the build.
set(CMAKE_FIND_ROOT_PATH /usr/${VIEWER_MINGW_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Link the GCC runtime and winpthread statically, so the .exe does not need
# libstdc++-6.dll, libgcc_s_seh-1.dll or libwinpthread-1.dll beside it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

# Lets `ctest` run the Windows test binary through Wine.
find_program(VIEWER_WINE_EXECUTABLE NAMES wine64 wine)
if(VIEWER_WINE_EXECUTABLE)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${VIEWER_WINE_EXECUTABLE})
endif()
