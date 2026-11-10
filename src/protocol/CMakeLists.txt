# Message structs are generated from the Linden Lab message template, which
# lives in the external/master-message-template submodule.
set(VIEWER_MESSAGE_TEMPLATE
  "${PROJECT_SOURCE_DIR}/external/master-message-template/message_template.msg"
  CACHE FILEPATH "message_template.msg to generate protocol code from")
if(NOT EXISTS "${VIEWER_MESSAGE_TEMPLATE}")
  message(FATAL_ERROR
    "Message template not found at ${VIEWER_MESSAGE_TEMPLATE}.\n"
    "Run: git submodule update --init")
endif()

add_executable(viewer_msggen generator/main.cpp)
target_link_libraries(viewer_msggen PRIVATE viewer_warnings)

set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(generated_hpp "${generated_dir}/viewer/protocol/Messages.hpp")
set(generated_cpp "${generated_dir}/Messages.cpp")
add_custom_command(
  OUTPUT "${generated_hpp}" "${generated_cpp}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${generated_dir}/viewer/protocol"
  COMMAND viewer_msggen "${VIEWER_MESSAGE_TEMPLATE}" "${generated_hpp}" "${generated_cpp}"
  DEPENDS viewer_msggen "${VIEWER_MESSAGE_TEMPLATE}"
  COMMENT "Generating message code from message_template.msg"
  VERBATIM
)

add_library(viewer_protocol STATIC
  CircuitManager.cpp
  MessageTypes.cpp
  Packet.cpp
  "${generated_cpp}"
  "${generated_hpp}"
)
add_library(viewer::protocol ALIAS viewer_protocol)

target_include_directories(viewer_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  "${generated_dir}"
)
target_link_libraries(viewer_protocol
  PUBLIC viewer::core viewer::net
  PRIVATE viewer_warnings
)
