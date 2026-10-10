# The updater must carry the macOS ASan fix into an existing product without
# replacing its product-owned presets or files.
cmake_minimum_required(VERSION 3.27)
file(REMOVE_RECURSE "${WORK}")
set(template "${WORK}/template")
set(product "${WORK}/product")
file(MAKE_DIRECTORY "${template}/framework/cmake" "${product}/framework/tools" "${product}/product")
file(WRITE "${template}/framework/VERSION.txt" "1.2.1\n")
file(WRITE "${template}/framework/cmake/Framework.cmake" "# updated framework\n")
file(WRITE "${product}/framework/VERSION" "1.1.0\n")
file(WRITE "${product}/product/product.cmake" "# product identity stays unchanged\n")
# Bootstrap as documented for 1.1.0, whose updater expects the old VERSION.
file(COPY "${FRAMEWORK}/tools/update_framework.cmake" DESTINATION "${product}/framework/tools")
file(WRITE "${product}/CMakePresets.json" [=[
{
  "version": 3,
  "configurePresets": [{"name": "custom", "binaryDir": "${sourceDir}/custom-build"}],
  "testPresets": [
    {"name": "asan", "environment": {
      "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1:log_path=/tmp/my-asan",
      "UBSAN_OPTIONS": "print_stacktrace=1"}},
    {"name": "custom", "environment": {"ASAN_OPTIONS": "detect_leaks=1"}}
  ]
}
]=])

function(run_update)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DFROM=${template}"
        -P "${product}/framework/tools/update_framework.cmake"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if (NOT status EQUAL 0)
        message(FATAL_ERROR "update failed: ${output}\n${error}")
    endif()
endfunction()

run_update()
file(READ "${product}/CMakePresets.json" presets)
string(JSON options GET "${presets}" testPresets 0 environment ASAN_OPTIONS)
string(JSON ubsan GET "${presets}" testPresets 0 environment UBSAN_OPTIONS)
string(JSON custom GET "${presets}" testPresets 1 environment ASAN_OPTIONS)
string(JSON binary_dir GET "${presets}" configurePresets 0 binaryDir)
if (NOT options STREQUAL "halt_on_error=1:log_path=/tmp/my-asan" OR
    NOT ubsan STREQUAL "print_stacktrace=1" OR NOT custom STREQUAL "detect_leaks=1" OR
    NOT binary_dir STREQUAL "\${sourceDir}/custom-build")
    message(FATAL_ERROR "ASan migration lost product-owned settings: ${presets}")
endif()
file(READ "${product}/framework/VERSION.txt" version)
file(READ "${product}/product/product.cmake" identity)
if (NOT version STREQUAL "1.2.1\n" OR EXISTS "${product}/framework/VERSION" OR
    NOT identity STREQUAL "# product identity stays unchanged\n")
    message(FATAL_ERROR "framework update did not preserve the product or replace the old framework")
endif()

# A second update must leave already-migrated presets byte-identical.
# The fixture framework is deliberately minimal, so reinstall the updater.
file(COPY "${FRAMEWORK}/tools/update_framework.cmake" DESTINATION "${product}/framework/tools")
run_update()
file(READ "${product}/CMakePresets.json" again)
if (NOT again STREQUAL presets)
    message(FATAL_ERROR "ASan migration is not idempotent")
endif()
message(STATUS "Existing-product ASan migration and preset preservation passed")
