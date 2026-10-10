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
message(STATUS "framework ${old} -> ${new}")
message(STATUS "Read framework/CHANGES.md, delete build/, and run every preset's tests.")
