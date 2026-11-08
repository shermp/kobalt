cmake_minimum_required(VERSION 3.25)
function(check_report status report context)
    string(JSON version GET "${report}" schema)
    string(JSON actual_context GET "${report}" context)
    string(JSON verified GET "${report}" sandbox_verified)
    string(JSON summary GET "${report}" summary)
    if(NOT version EQUAL 1 OR NOT actual_context STREQUAL context OR verified)
        message(FATAL_ERROR "Incorrect diagnostic scope: ${report}")
    endif()
    if(status EQUAL 0 AND NOT summary STREQUAL "primitives-available"
       OR status EQUAL 77 AND NOT summary STREQUAL "unavailable"
       OR NOT status MATCHES "^(0|77)$")
        message(FATAL_ERROR "Diagnostic status mismatch: ${status}: ${report}")
    endif()
    string(JSON count LENGTH "${report}" tests)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON name GET "${report}" tests ${index} name)
        string(JSON result GET "${report}" tests ${index} result)
        string(JSON number GET "${report}" tests ${index} errno)
        if(result STREQUAL "ok" AND NOT number EQUAL 0
           OR result STREQUAL "unavailable" AND number EQUAL 0)
            message(FATAL_ERROR "Missing diagnostic errno: ${report}")
        endif()
        if(name STREQUAL "memfd")
            set(memfd_result "${result}" PARENT_SCOPE)
            set(memfd_errno "${number}" PARENT_SCOPE)
        endif()
        if(name STREQUAL "combined-namespaces")
            set(namespaces_result "${result}" PARENT_SCOPE)
            set(namespaces_errno "${number}" PARENT_SCOPE)
        endif()
    endforeach()
endfunction()
execute_process(COMMAND "${PROBE}" --context native-host RESULT_VARIABLE status
    OUTPUT_VARIABLE report ERROR_VARIABLE error TIMEOUT 20)
check_report("${status}" "${report}" native-host)
foreach(mode missing permission hang)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_PRELOAD=${SHIM}"
        "KOBALT_TEST_PROBE_FAILURE=${mode}" "${PROBE}" --context physical-device
        RESULT_VARIABLE status OUTPUT_VARIABLE report ERROR_VARIABLE error TIMEOUT 20)
    check_report("${status}" "${report}" physical-device)
    if(mode STREQUAL "missing")
        set(expected 38) # Linux ENOSYS
    elseif(mode STREQUAL "permission")
        set(expected 1) # Linux EPERM
    else()
        set(expected 110) # Linux ETIMEDOUT
    endif()
    set(namespace_expected "${expected}")
    if(mode STREQUAL "hang")
        set(namespace_expected 38)
    endif()
    if(NOT status EQUAL 77 OR NOT memfd_result STREQUAL "unavailable"
       OR NOT namespaces_result STREQUAL "unavailable"
       OR NOT memfd_errno EQUAL expected OR NOT namespaces_errno EQUAL namespace_expected)
        message(FATAL_ERROR "Kernel limitation not preserved: ${mode}: ${status}: ${report}\n${error}")
    endif()
endforeach()
execute_process(COMMAND "${PROBE}" --context unknown RESULT_VARIABLE status
    OUTPUT_VARIABLE report ERROR_QUIET TIMEOUT 5)
if(NOT status EQUAL 2 OR NOT report STREQUAL "")
    message(FATAL_ERROR "Invalid context accepted")
endif()
