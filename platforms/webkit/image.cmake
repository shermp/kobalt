file(READ "${CMAKE_CURRENT_LIST_DIR}/../../dependencies/webkit/build-image.json" lock)
string(JSON image GET "${lock}" image)
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${image}")
