execute_process(
  COMMAND ldd ${EXECUTABLES}
  RESULT_VARIABLE status
  OUTPUT_VARIABLE libraries
  ERROR_VARIABLE error)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "Cannot inspect linked libraries: ${error}")
endif()
if(libraries MATCHES "libasound")
  message(
    FATAL_ERROR "Lyren must use PipeWire without linking ALSA:\n${libraries}")
endif()
