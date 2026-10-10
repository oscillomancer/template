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

cmake_minimum_required(VERSION 3.21)

if (NOT FROM)
    message(FATAL_ERROR "update_framework: -DFROM=<path to the template> is required")
endif()
get_filename_component(from "${FROM}/framework" ABSOLUTE)
get_filename_component(here "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if (NOT EXISTS "${from}/VERSION")
    message(FATAL_ERROR "update_framework: ${from} is not a framework directory")
endif()
if (from STREQUAL here)
    message(FATAL_ERROR "update_framework: this is the template's own framework")
endif()
if (NOT EXISTS "${here}/../product/product.cmake")
    message(FATAL_ERROR "update_framework: run this from a product built on the framework")
endif()

file(READ "${here}/VERSION" old)
file(READ "${from}/VERSION" new)
string(STRIP "${old}" old)
string(STRIP "${new}" new)

get_filename_component(root "${here}/.." ABSOLUTE)
file(REMOVE_RECURSE "${here}")
file(COPY "${from}" DESTINATION "${root}")
message(STATUS "framework ${old} -> ${new}")
message(STATUS "Read framework/CHANGES.md, delete build/, and run every preset's tests.")
