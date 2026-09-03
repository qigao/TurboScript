if(NOT DEFINED CLI OR NOT DEFINED INVALID_SCRIPT)
  message(FATAL_ERROR "CLI and INVALID_SCRIPT are required")
endif()

function(verify_cli_failure mode)
  if(mode STREQUAL "eval")
    execute_process(
      COMMAND "${CLI}" --eval "var invalid = ;"
      RESULT_VARIABLE result
      OUTPUT_VARIABLE output
      ERROR_VARIABLE error)
  elseif(mode STREQUAL "file")
    execute_process(
      COMMAND "${CLI}" --file "${INVALID_SCRIPT}"
      RESULT_VARIABLE result
      OUTPUT_VARIABLE output
      ERROR_VARIABLE error)
  else()
    message(FATAL_ERROR "unknown CLI verification mode: ${mode}")
  endif()

  if(result EQUAL 0)
    message(FATAL_ERROR "${mode} accepted an invalid script")
  endif()
  if(NOT error MATCHES "TurboScript: .+")
    message(FATAL_ERROR
      "${mode} did not report a diagnostic on stderr\nstdout: ${output}\nstderr: ${error}")
  endif()
endfunction()

verify_cli_failure(eval)
verify_cli_failure(file)
