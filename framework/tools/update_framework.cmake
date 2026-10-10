# Replaces this product's framework/ with the template's, so a fix made in
# the template reaches the product unchanged. Run from the product's root:
#
#   cmake -DFROM=../template -P framework/tools/update_framework.cmake
#
# framework/ is never edited inside a product: it is the template's,
# byte for byte. A product that needs a framework change makes it in the
# template, tests it there, and brings it over with this.
#
# Afterwards: read framework/CHANGES.md for anything a product must do,
# delete the build directories, and run every preset's tests.

cmake_minimum_required(VERSION 3.27)

if (NOT FROM)
    message(FATAL_ERROR "update_framework: -DFROM=<path to the template> is required")
endif()
# Real paths: a FROM reached through a symlink must still be recognised as
# this very framework, or the copy below would delete its own source.
get_filename_component(from "${FROM}/framework" REALPATH)
get_filename_component(here "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
# The version file is VERSION.txt from 1.2.0; 1.1.0 and earlier called it
# VERSION, which #include <version> found on case-insensitive filesystems.
function(framework_version_file dir out)
    if (EXISTS "${dir}/VERSION.txt")
        set(${out} "${dir}/VERSION.txt" PARENT_SCOPE)
    elseif (EXISTS "${dir}/VERSION")
        set(${out} "${dir}/VERSION" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()
framework_version_file("${from}" from_version)
if (NOT from_version)
    message(FATAL_ERROR "update_framework: ${from} is not a framework directory")
endif()
string(FIND "${from}/" "${here}/" from_in_here)
string(FIND "${here}/" "${from}/" here_in_from)
if (from_in_here EQUAL 0 OR here_in_from EQUAL 0)
    message(FATAL_ERROR "update_framework: ${from} and ${here} are the same framework or nested")
endif()
if (NOT EXISTS "${here}/../product/product.cmake")
    message(FATAL_ERROR "update_framework: run this from a product built on the framework")
endif()

framework_version_file("${here}" here_version)
set(old "unknown")
if (here_version)
    file(READ "${here_version}" old)
endif()
file(READ "${from_version}" new)
string(STRIP "${old}" old)
string(STRIP "${new}" new)

# The sanitizer preset is product-owned, outside framework/. Migrate only
# the option that macOS rejects (1.2.0 removed detect_leaks=1 from the
# template's asan test preset), retaining every other preset and option.
# CMake's JSON support finds the value, but writing the document back through
# it would re-serialize the whole file with its keys sorted, so the edit is
# made to the file's text instead: the one string literal changes and every
# other byte, formatting included, stays as the product wrote it.
function(migrate_asan_preset root)
    set(path "${root}/CMakePresets.json")
    if (NOT EXISTS "${path}")
        return()
    endif()
    file(READ "${path}" text)
    string(JSON count ERROR_VARIABLE error LENGTH "${text}" testPresets)
    if (error OR count EQUAL 0)
        return()
    endif()
    math(EXPR last "${count} - 1")
    foreach(i RANGE 0 ${last})
        string(JSON name ERROR_VARIABLE error GET "${text}" testPresets ${i} name)
        if (error OR NOT name STREQUAL "asan")
            continue()
        endif()
        string(JSON before ERROR_VARIABLE error GET "${text}" testPresets ${i} environment ASAN_OPTIONS)
        if (error)
            continue()
        endif()
        set(after "${before}")
        while (after MATCHES "(^|:)detect_leaks=1(:|$)")
            string(REGEX REPLACE "(^|:)detect_leaks=1:" "\\1" after "${after}")
            string(REGEX REPLACE "(^|:)detect_leaks=1$" "" after "${after}")
        endwhile()
        if (after STREQUAL before)
            continue()
        endif()
        # The old value as a JSON string literal, and how often it appears.
        # Anything but exactly once (another preset with the same options, or
        # an unusual escape) is left to the product's owner.
        foreach(var before after)
            string(REPLACE "\\" "\\\\" literal_${var} "${${var}}")
            string(REPLACE "\"" "\\\"" literal_${var} "${literal_${var}}")
            set(literal_${var} "\"${literal_${var}}\"")
        endforeach()
        string(REPLACE "${literal_before}" "" without "${text}")
        string(LENGTH "${text}" full)
        string(LENGTH "${without}" rest)
        string(LENGTH "${literal_before}" one)
        math(EXPR occurrences "(${full} - ${rest}) / ${one}")
        if (NOT occurrences EQUAL 1)
            message(WARNING "update_framework: remove detect_leaks=1 from ASAN_OPTIONS in the asan "
                            "test preset of ${path} by hand; macOS's AddressSanitizer rejects it")
            return()
        endif()
        string(REPLACE "${literal_before}" "${literal_after}" edited "${text}")
        string(JSON check ERROR_VARIABLE error GET "${edited}" testPresets ${i} environment ASAN_OPTIONS)
        if (error OR NOT check STREQUAL after)
            message(WARNING "update_framework: could not edit ${path}; remove detect_leaks=1 from "
                            "the asan test preset's ASAN_OPTIONS by hand")
            return()
        endif()
        file(WRITE "${path}" "${edited}")
        message(STATUS "Removed macOS-incompatible detect_leaks=1 from the asan test preset")
        return()
    endforeach()
endfunction()

# Copy beside the old framework first and swap only once the copy is
# complete, so a failed or interrupted copy leaves the product as it was.
get_filename_component(root "${here}/.." ABSOLUTE)
set(staging "${root}/.framework-update")
set(previous "${root}/.framework-previous")
file(REMOVE_RECURSE "${staging}" "${previous}")
file(MAKE_DIRECTORY "${staging}")
file(COPY "${from}/" DESTINATION "${staging}")
framework_version_file("${staging}" staged_version)
if (NOT staged_version OR NOT EXISTS "${staging}/cmake/Framework.cmake")
    file(REMOVE_RECURSE "${staging}")
    message(FATAL_ERROR "update_framework: the copy is incomplete; framework/ is unchanged")
endif()
file(RENAME "${here}" "${previous}")
file(RENAME "${staging}" "${here}")
file(REMOVE_RECURSE "${previous}")
if (new VERSION_GREATER_EQUAL "1.2.0")
    migrate_asan_preset("${root}")
endif()
message(STATUS "framework ${old} -> ${new}")
message(STATUS "Read framework/CHANGES.md, delete build/, and run every preset's tests.")
