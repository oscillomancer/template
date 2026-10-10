# Creates a new product from this template.
#
#   cmake -DNAME="Loupe" -DID=com.suppleaudio.loupe -DCODE=LoUp -DDEST=../loupe \
#         [-DKIND=effect|instrument] [-DDESCRIPTION="A parametric EQ."] \
#         [-DLICENSE=GPL-3.0-or-later|MIT] \
#         -P framework/tools/new_product.cmake
#
# NAME is the display name. ID is the permanent reverse-DNS identifier. CODE
# is the permanent four-character AU subtype, also used as the state tag.
# KIND picks the starting point: the template's effect, or the instrument in
# framework/examples/instrument. LICENSE defaults to GPL-3.0-or-later, which
# Supple Audio products use; the framework/ directory stays MIT either way.
#
# The result builds and passes every test as it stands. Then: describe the
# product in product/, starting with Params.h and Processor.h.

cmake_minimum_required(VERSION 3.27)

get_filename_component(template "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

foreach(var NAME ID CODE DEST)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "new_product: -D${var}=... is required (see the top of this file)")
    endif()
endforeach()
if (NOT KIND)
    set(KIND effect)
endif()
if (NOT LICENSE)
    set(LICENSE GPL-3.0-or-later)
endif()
if (NOT KIND MATCHES "^(effect|instrument)$")
    message(FATAL_ERROR "new_product: KIND must be effect or instrument")
endif()
if (NOT LICENSE MATCHES "^(GPL-3.0-or-later|MIT)$")
    message(FATAL_ERROR "new_product: LICENSE must be GPL-3.0-or-later or MIT")
endif()
# These land inside quoted CMake strings and C++ string literals: nothing
# that either would interpret.
foreach(text NAME DESCRIPTION)
    if ("${${text}}" MATCHES "[\"\\\\$;@\n\r]")
        message(FATAL_ERROR "new_product: ${text} may not contain quotes, backslashes, $, ;, @ or line breaks")
    endif()
endforeach()
if (NOT ID MATCHES "^[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)+$")
    message(FATAL_ERROR "new_product: ID must be reverse-DNS, e.g. com.suppleaudio.product")
endif()
if (NOT CODE MATCHES "^[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]$")
    message(FATAL_ERROR "new_product: CODE must be exactly four letters or digits")
endif()
if (CODE STREQUAL "StFx" OR CODE STREQUAL "ToNe")
    message(FATAL_ERROR "new_product: ${CODE} belongs to the template's own examples; choose a new code")
endif()

get_filename_component(dest "${DEST}" ABSOLUTE)
if (EXISTS "${dest}")
    file(GLOB existing "${dest}/*" "${dest}/.*")
    if (existing)
        message(FATAL_ERROR "new_product: ${dest} exists and is not empty")
    endif()
endif()
file(MAKE_DIRECTORY "${dest}")

string(TIMESTAMP year "%Y")
file(READ "${template}/framework/VERSION.txt" framework_version)
string(STRIP "${framework_version}" framework_version)

# --- copy ------------------------------------------------------------------------------

file(COPY "${template}/framework" DESTINATION "${dest}")
foreach(file CMakeLists.txt CMakePresets.json .gitignore)
    file(COPY "${template}/${file}" DESTINATION "${dest}")
endforeach()
if (KIND STREQUAL "instrument")
    file(COPY "${template}/framework/examples/instrument/" DESTINATION "${dest}/product")
else()
    file(COPY "${template}/product/" DESTINATION "${dest}/product")
endif()

# --- identity ----------------------------------------------------------------------------

set(product_cmake "${dest}/product/product.cmake")
file(READ "${product_cmake}" text)
function(set_line var value)
    string(REGEX REPLACE "set\\(${var}( +)\"[^\"]*\"\\)" "set(${var}\\1\"${value}\")" text "${text}")
    set(text "${text}" PARENT_SCOPE)
endfunction()
set_line(PRODUCT_NAME "${NAME}")
set_line(PRODUCT_ID "${ID}")
set_line(PRODUCT_VERSION "0.1.0")
set_line(PRODUCT_AU_SUBTYPE "${CODE}")
set_line(PRODUCT_STATE_TAG "${CODE}")
if (DESCRIPTION)
    set_line(PRODUCT_DESCRIPTION "${DESCRIPTION}")
else()
    set(DESCRIPTION "")
endif()
if (LICENSE STREQUAL "GPL-3.0-or-later")
    set_line(PRODUCT_COPYRIGHT "Copyright (C) ${year} Supple Audio")
else()
    set_line(PRODUCT_COPYRIGHT "Copyright (c) ${year} Supple Audio")
endif()
file(WRITE "${product_cmake}" "${text}")

# --- license -------------------------------------------------------------------------------

if (LICENSE STREQUAL "GPL-3.0-or-later")
    file(COPY_FILE "${template}/framework/tools/licenses/GPL-3.0-or-later.txt" "${dest}/LICENSE")
    # The product's own sources take the product's license; framework/ keeps
    # its MIT headers, which is both correct and what keeps it byte-identical
    # with the template for updates.
    file(GLOB_RECURSE product_sources "${dest}/product/*.h" "${dest}/product/*.cpp" "${dest}/product/*.cmake")
    foreach(source ${product_sources})
        file(READ "${source}" content)
        string(REPLACE "SPDX-License-Identifier: MIT" "SPDX-License-Identifier: GPL-3.0-or-later" content "${content}")
        string(REGEX REPLACE "Copyright \\(c\\) [0-9]+ Supple Audio" "Copyright (C) ${year} Supple Audio" content "${content}")
        file(WRITE "${source}" "${content}")
    endforeach()
    set(license_text "${NAME} is free software, licensed under the GNU General Public License version 3 or (at your option) any later version; see [LICENSE](LICENSE).")
else()
    file(READ "${template}/LICENSE" mit)
    string(REGEX REPLACE "Copyright \\(c\\) [0-9]+ Supple Audio" "Copyright (c) ${year} Supple Audio" mit "${mit}")
    file(WRITE "${dest}/LICENSE" "${mit}")
    set(license_text "${NAME} is MIT licensed; see [LICENSE](LICENSE).")
endif()

# --- product documents -----------------------------------------------------------------------

file(WRITE "${dest}/README.md" "# ${NAME}

${DESCRIPTION}

Built on the Supple Audio plugin framework ${framework_version}. Everything that
makes this product what it is lives in `product/`; `framework/` is shared,
unchanged, with every product, and is updated from the template with
`framework/tools/update_framework.cmake`.

## Building

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

See [framework/docs/BUILDING.md](framework/docs/BUILDING.md) for platforms,
sanitizers, validators and installing.

## License

${license_text} The framework in `framework/` is MIT licensed
([framework/LICENSE](framework/LICENSE)). Third-party notices are in
[framework/NOTICES.txt](framework/NOTICES.txt) and ship inside every built
plugin.
")

file(WRITE "${dest}/AGENTS.md" "# ${NAME}: instructions for agents

Read [framework/AGENTS.md](framework/AGENTS.md) first. It is the contract for
every product built on this framework: where code goes, what may not be
changed, and what \"done\" means.

## This product

Keep this section current as the product takes shape: what the DSP does and
why, each parameter's meaning, latency and tail behaviour, anything that
needs a restart, and every measured claim with the test that measures it.
")

message(STATUS "Created ${NAME} (${ID}, ${CODE}, ${KIND}, ${LICENSE}) at ${dest}")
message(STATUS "Next:")
message(STATUS "  cd ${dest} && git init")
message(STATUS "  cmake --preset release && cmake --build --preset release && ctest --preset release")
