find_program(GLSLC glslc REQUIRED)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

# Compiles GLSL to SPIR-V; sources of <target> pull the result in with #embed
# "<name>.spv".
function(add_shaders target)
  foreach(src ${ARGN})
    get_filename_component(name ${src} NAME)
    set(out ${SHADER_OUT}/${name}.spv)
    add_custom_command(
      OUTPUT ${out}
      COMMAND ${GLSLC} -O --target-env=vulkan1.3 -MD -MF ${out}.d
              ${CMAKE_SOURCE_DIR}/${src} -o ${out}
      DEPENDS ${CMAKE_SOURCE_DIR}/${src}
      DEPFILE ${out}.d
      VERBATIM)
    list(APPEND outputs ${out})
  endforeach()
  add_custom_target(${target}_shaders DEPENDS ${outputs})
  add_dependencies(${target} ${target}_shaders)
  target_compile_options(${target} PRIVATE --embed-dir=${SHADER_OUT})
endfunction()
