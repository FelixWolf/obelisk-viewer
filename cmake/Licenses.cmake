# The licenses of what's in the viewer (shown on the main menu's About page).
#
# viewer_license(<name> <file>...) notes a part of the viewer and the files
# holding its license; viewer_write_licenses(<output>) writes them all into
# one file, each starting with an ASCII record separator (0x1e), the name
# and a newline. Only what ships goes in (not build tools or the tests'
# libraries). Changing a listed file configures again.

function(viewer_license name)
  foreach(file IN LISTS ARGN)
    if(NOT EXISTS "${file}")
      message(FATAL_ERROR "viewer_license(${name}): no ${file}")
    endif()
  endforeach()
  set_property(GLOBAL APPEND PROPERTY VIEWER_LICENSE_NAMES "${name}")
  string(REPLACE ";" "|" files "${ARGN}")
  set_property(GLOBAL APPEND PROPERTY VIEWER_LICENSE_FILES "${files}")
endfunction()

function(viewer_write_licenses output)
  get_property(names GLOBAL PROPERTY VIEWER_LICENSE_NAMES)
  get_property(lists GLOBAL PROPERTY VIEWER_LICENSE_FILES)
  string(ASCII 30 separator)
  set(text "")
  list(LENGTH names count)
  math(EXPR last "${count} - 1")
  foreach(i RANGE ${last})
    list(GET names ${i} name)
    list(GET lists ${i} files)
    string(REPLACE "|" ";" files "${files}")
    string(APPEND text "${separator}${name}\n")
    foreach(file IN LISTS files)
      file(READ "${file}" body)
      string(APPEND text "${body}\n")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    endforeach()
  endforeach()
  file(WRITE "${output}.tmp" "${text}")
  file(COPY_FILE "${output}.tmp" "${output}" ONLY_IF_DIFFERENT)
  file(REMOVE "${output}.tmp")
endfunction()
