# The media host (main.cpp), in bin/media beside libcef and its resources:
# Chromium finds those beside the program, and starts this same program
# again for its own processes.
#
# Linux: a program. Windows: a DLL that CEF's bootstrap.exe, copied as
# obelisk-media.exe, loads (it loads <its own name>.dll): Chromium's sandbox
# only works for programs built with Chromium, so the bootstrap is the
# program and we're its module (CEF 138 and later).
set(media_dir "${CMAKE_BINARY_DIR}/bin/media")
if(WIN32)
  add_library(viewer_media_host SHARED main.cpp)
  set_target_properties(viewer_media_host PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${VIEWER_EXECUTABLE}-media"
    RUNTIME_OUTPUT_DIRECTORY "${media_dir}"
  )
  # The GCC runtime inside it (as the viewer's), so nothing else is needed
  # beside it.
  target_link_options(viewer_media_host PRIVATE -static -static-libgcc -static-libstdc++)
else()
  add_executable(viewer_media_host main.cpp)
  set_target_properties(viewer_media_host PROPERTIES
    OUTPUT_NAME "${VIEWER_EXECUTABLE}-media"
    RUNTIME_OUTPUT_DIRECTORY "${media_dir}"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()
target_link_libraries(viewer_media_host PRIVATE viewer_cef_wrapper viewer_media_ipc viewer_warnings)
# Built as libcef is (a release build), whatever the rest is.
target_compile_definitions(viewer_media_host PRIVATE NDEBUG)
# CEF's headers aren't held to our warnings; ours still are.
target_compile_options(viewer_media_host PRIVATE -Wno-unused-parameter)

# libcef and its files beside it.
if(WIN32)
  set(cef_files
    "${VIEWER_CEF_DIR}/Release/libcef.dll"
    "${VIEWER_CEF_DIR}/Release/chrome_elf.dll"
    "${VIEWER_CEF_DIR}/Release/d3dcompiler_47.dll"
    "${VIEWER_CEF_DIR}/Release/vk_swiftshader.dll"
    "${VIEWER_CEF_DIR}/Release/vk_swiftshader_icd.json"
    "${VIEWER_CEF_DIR}/Release/vulkan-1.dll"
    "${VIEWER_CEF_DIR}/Release/v8_context_snapshot.bin"
  )
  # (dxcompiler.dll, 26 MB, is only WebGPU's: left out.)
else()
  set(cef_files
    "${VIEWER_CEF_DIR}/Release/libcef.so"
    "${VIEWER_CEF_DIR}/Release/libvk_swiftshader.so"
    "${VIEWER_CEF_DIR}/Release/libvulkan.so.1"
    "${VIEWER_CEF_DIR}/Release/v8_context_snapshot.bin"
    "${VIEWER_CEF_DIR}/Release/vk_swiftshader_icd.json"
  )
endif()
list(APPEND cef_files
  "${VIEWER_CEF_DIR}/Resources/chrome_100_percent.pak"
  "${VIEWER_CEF_DIR}/Resources/chrome_200_percent.pak"
  "${VIEWER_CEF_DIR}/Resources/icudtl.dat"
  "${VIEWER_CEF_DIR}/Resources/resources.pak"
  "${VIEWER_CEF_DIR}/LICENSE.txt"
  "${VIEWER_CEF_DIR}/CREDITS.html"
)
# Only the locale it's told to use (CefSettings::locale).
set(cef_locale "${VIEWER_CEF_DIR}/Resources/locales/en-US.pak")

# In the build tree: links (libcef alone is over a gigabyte unstripped on
# Linux); copies on Windows, where Wine and Windows both run them.
file(MAKE_DIRECTORY "${media_dir}/locales")
foreach(f IN LISTS cef_files cef_locale)
  get_filename_component(name "${f}" NAME)
  set(to "${media_dir}/${name}")
  if(f STREQUAL cef_locale)
    set(to "${media_dir}/locales/${name}")
  endif()
  if(WIN32)
    file(COPY_FILE "${f}" "${to}" ONLY_IF_DIFFERENT)
  else()
    file(CREATE_LINK "${f}" "${to}" SYMBOLIC)
  endif()
endforeach()

# (A DLL is a RUNTIME file, a program too; each kind names its component.)
install(TARGETS viewer_media_host
        RUNTIME DESTINATION media COMPONENT viewer
        LIBRARY DESTINATION media COMPONENT viewer)
install(FILES ${cef_files} DESTINATION media COMPONENT viewer)
install(FILES "${cef_locale}" DESTINATION media/locales COMPONENT viewer)
if(WIN32)
  # The program: CEF's bootstrap, under our name.
  set(bootstrap "${VIEWER_CEF_DIR}/Release/bootstrap.exe")
  file(COPY_FILE "${bootstrap}" "${media_dir}/${VIEWER_EXECUTABLE}-media.exe" ONLY_IF_DIFFERENT)
  install(FILES "${bootstrap}" DESTINATION media RENAME "${VIEWER_EXECUTABLE}-media.exe"
          PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                      WORLD_READ WORLD_EXECUTE
          COMPONENT viewer)
else()
  # libcef ships stripped with the rest (--strip only strips our targets).
  install(CODE "
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND \"${CMAKE_STRIP}\" --strip-unneeded
                      \"\${CMAKE_INSTALL_PREFIX}/media/libcef.so\")
    endif()" COMPONENT viewer)
endif()
