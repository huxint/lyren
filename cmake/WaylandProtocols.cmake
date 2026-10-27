find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(WP_DIR ${wayland_protocols_SOURCE_DIR})
set(PROTOCOL_OUT ${CMAKE_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${PROTOCOL_OUT})

# Generates client header + glue code for each XML and adds them to <target>.
function(add_wayland_protocols target)
  foreach(xml ${ARGN})
    get_filename_component(name ${xml} NAME_WE)
    set(header ${PROTOCOL_OUT}/${name}-client.h)
    set(code ${PROTOCOL_OUT}/${name}.c)
    add_custom_command(
      OUTPUT ${header} ${code}
      COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
      COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
      DEPENDS ${xml}
      VERBATIM)
    target_sources(${target} PRIVATE ${header} ${code})
  endforeach()
  target_include_directories(${target} PUBLIC ${PROTOCOL_OUT})
endfunction()
