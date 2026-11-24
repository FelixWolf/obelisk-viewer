# What the viewer is called: the one place to rename it.
#
# Edit these, or give them to one build (cmake --preset debug
# -DVIEWER_NAME="My Viewer"); a -D stays in that build directory's cache and
# wins over this file until cleared (-UVIEWER_NAME). They're ordinary
# variables rather than cache entries so that an edit here reaches build
# directories that already exist.
#
# Code gets them from viewer/core/Branding.hpp, the interface's documents as
# {{ appName }}, dist.sh from <build>/branding.txt. Names only developers see
# (the viewer:: namespace, CMake targets, VIEWER_* options, the "viewer"
# install component, keys in cached textures) aren't branding and stay.

macro(viewer_branding name)
  if(NOT DEFINED ${name} OR "${${name}}" STREQUAL "")
    set(${name} "${ARGN}")
  endif()
endmacro()

# What people see: the window's title, the interface, the installer.
viewer_branding(VIEWER_NAME "Obelisk Viewer")
# The default for the three after it.
viewer_branding(VIEWER_SHORT_NAME "obelisk")

# The program's file name (without .exe) and its log's.
viewer_branding(VIEWER_EXECUTABLE "${VIEWER_SHORT_NAME}")
# The folder under ~/.config, ~/.cache, %APPDATA%, ..., and the name saved
# passwords are under in the system's credential store. Changing it starts
# afresh: nothing is moved from the old one.
viewer_branding(VIEWER_DIRECTORY "${VIEWER_SHORT_NAME}")
# The channel Second Life's login is told.
viewer_branding(VIEWER_CHANNEL "${VIEWER_SHORT_NAME}")

# The name where spaces can't go: the installer's and packages' id (Velopack
# takes a changed one for another program), and the HTTP User-Agent.
string(REGEX REPLACE "[^A-Za-z0-9.-]" "" viewer_default_id "${VIEWER_NAME}")
viewer_branding(VIEWER_PACKAGE_ID "${viewer_default_id}")

foreach(name VIEWER_NAME VIEWER_CHANNEL)
  if("${${name}}" MATCHES "[\\\\\"$@;|\n]")
    message(FATAL_ERROR "${name} can't have any of \\ \" $ @ ; | in it: ${${name}}")
  endif()
endforeach()
foreach(name VIEWER_EXECUTABLE VIEWER_DIRECTORY VIEWER_PACKAGE_ID)
  if(NOT "${${name}}" MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
    message(FATAL_ERROR "${name} must be letters, digits, '.', '_' and '-': \"${${name}}\"")
  endif()
endforeach()

# For dist.sh.
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/branding.txt" @ONLY CONTENT
"name=@VIEWER_NAME@
executable=@VIEWER_EXECUTABLE@
package_id=@VIEWER_PACKAGE_ID@
version=@PROJECT_VERSION@
")
