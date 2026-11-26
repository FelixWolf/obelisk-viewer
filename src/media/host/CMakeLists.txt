# The media host program (main.cpp), in bin/media beside libcef and its
# resources: Chromium finds those beside the program, and starts this same
# program again for its own processes.
add_executable(viewer_media_host main.cpp)
set_target_properties(viewer_media_host PROPERTIES
  OUTPUT_NAME "${VIEWER_EXECUTABLE}-media"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/media"
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN"
)
target_link_libraries(viewer_media_host PRIVATE viewer_cef_wrapper viewer_media_ipc viewer_warnings)
# Built as libcef is (a release build), whatever the rest is.
target_compile_definitions(viewer_media_host PRIVATE NDEBUG)
# CEF's headers aren't held to our warnings; ours still are.
target_compile_options(viewer_media_host PRIVATE -Wno-unused-parameter)

# libcef and its files beside it: links in the build tree (libcef alone is
# over a gigabyte unstripped), copies in what's installed.
set(cef_files
  "${VIEWER_CEF_DIR}/Release/libcef.so"
  "${VIEWER_CEF_DIR}/Release/libvk_swiftshader.so"
  "${VIEWER_CEF_DIR}/Release/libvulkan.so.1"
  "${VIEWER_CEF_DIR}/Release/v8_context_snapshot.bin"
  "${VIEWER_CEF_DIR}/Release/vk_swiftshader_icd.json"
  "${VIEWER_CEF_DIR}/Resources/chrome_100_percent.pak"
  "${VIEWER_CEF_DIR}/Resources/chrome_200_percent.pak"
  "${VIEWER_CEF_DIR}/Resources/icudtl.dat"
  "${VIEWER_CEF_DIR}/Resources/resources.pak"
  "${VIEWER_CEF_DIR}/LICENSE.txt"
  "${VIEWER_CEF_DIR}/CREDITS.html"
)
# Only the locale it's told to use (CefSettings::locale).
set(cef_locale "${VIEWER_CEF_DIR}/Resources/locales/en-US.pak")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/media/locales")
foreach(f IN LISTS cef_files)
  get_filename_component(name "${f}" NAME)
  file(CREATE_LINK "${f}" "${CMAKE_BINARY_DIR}/bin/media/${name}" SYMBOLIC)
endforeach()
file(CREATE_LINK "${cef_locale}" "${CMAKE_BINARY_DIR}/bin/media/locales/en-US.pak" SYMBOLIC)

install(TARGETS viewer_media_host RUNTIME DESTINATION media COMPONENT viewer)
install(FILES ${cef_files} DESTINATION media COMPONENT viewer)
install(FILES "${cef_locale}" DESTINATION media/locales COMPONENT viewer)
# libcef ships stripped with the rest (--strip only strips our targets).
install(CODE "
  if(CMAKE_INSTALL_DO_STRIP)
    execute_process(COMMAND \"${CMAKE_STRIP}\" --strip-unneeded
                    \"\${CMAKE_INSTALL_PREFIX}/media/libcef.so\")
  endif()" COMPONENT viewer)
