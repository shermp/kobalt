cmake_minimum_required(VERSION 3.25)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
if(EXISTS "${OUTPUT}")
    message(FATAL_ERROR "SDK-recovery test scratch remains: ${OUTPUT}")
endif()
file(MAKE_DIRECTORY "${OUTPUT}")
file(WRITE "${OUTPUT}/damaged.tar" "not the pinned SDK")
file(WRITE "${OUTPUT}/engine" "#!/bin/sh\nprintf invoked > \"$KOBALT_ENGINE_MARKER\"\nexit 1\n")
file(CHMOD "${OUTPUT}/engine" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "CONTAINER_ENGINE=${OUTPUT}/engine" "KOBALT_ENGINE_MARKER=${OUTPUT}/engine-was-invoked"
    "${CMAKE_COMMAND}" -DACTION=RESTORE "-DARCHIVE=${OUTPUT}/damaged.tar"
    -P "${root}/platforms/webkit/sdk-image.cmake"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "SDK recovery archive checksum mismatch")
    message(FATAL_ERROR "Damaged SDK accepted: ${status}\n${output}\n${error}")
endif()
if(EXISTS "${OUTPUT}/engine-was-invoked")
    message(FATAL_ERROR "A damaged SDK reached the container engine")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPATHS=${OUTPUT}" -P "${root}/platforms/cleanup.cmake"
    COMMAND_ERROR_IS_FATAL ANY)
