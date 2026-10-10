# The framework build: every target, test and install rule, generated from
# product/product.cmake. Nothing in here names a product.

set(SUPPLE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(SUPPLE_FRAMEWORK ${SUPPLE_ROOT}/framework)
set(SUPPLE_PRODUCT ${SUPPLE_ROOT}/product)
set(SUPPLE_GENERATED ${CMAKE_BINARY_DIR}/generated)
# VERSION.txt, never VERSION: framework/ is an include root, and on a
# case-insensitive filesystem (macOS, Windows) a file called VERSION is what
# the standard library's own #include <version> finds.
file(READ ${SUPPLE_FRAMEWORK}/VERSION.txt SUPPLE_FRAMEWORK_VERSION)
string(STRIP "${SUPPLE_FRAMEWORK_VERSION}" SUPPLE_FRAMEWORK_VERSION)
include(${SUPPLE_FRAMEWORK}/cmake/CheckIncludeRoots.cmake)
supple_check_include_roots(${SUPPLE_FRAMEWORK} ${SUPPLE_FRAMEWORK}/test ${SUPPLE_PRODUCT})
message(STATUS "${PRODUCT_NAME} ${PRODUCT_VERSION} on framework ${SUPPLE_FRAMEWORK_VERSION}")

# Bundle staging, AU preparation, install rules and tests all name
# plugins/<Name>.<ext>; a multi-config generator (Visual Studio, Xcode, Ninja
# Multi-Config) adds a per-configuration directory under that and, with
# clap-wrapper, shares some bundles between configurations. One build
# directory per configuration, which the presets already give.
get_property(supple_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (supple_multi_config)
    message(FATAL_ERROR "${CMAKE_GENERATOR} is a multi-configuration generator; use Ninja or Makefiles "
                        "(on Windows, from a Visual Studio developer prompt: cmake --preset release -G Ninja)")
endif()

option(SUPPLE_BUILD_PLUGIN "Build the CLAP, VST3 and AUv2 plugins" ON)
option(SUPPLE_BUILD_GUI "Build the editor" ON)
option(SUPPLE_BUILD_TESTS "Build the tests, tools and probes" ON)
option(SUPPLE_WARNINGS_AS_ERRORS "Treat warnings in framework and product code as errors" OFF)
set(SUPPLE_SANITIZE "" CACHE STRING "Instrument our code: empty, address (with undefined), or thread")
set_property(CACHE SUPPLE_SANITIZE PROPERTY STRINGS "" address thread)
if (APPLE)
    # Apple deprecated OpenGL in 2018. It still works, but the direction is
    # not in doubt: macOS renders through Metal, GL everywhere else.
    option(SUPPLE_METAL "Render the editor through Metal" ON)
else()
    set(SUPPLE_METAL OFF)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

function(supple_sanitize target)
    if (MSVC OR SUPPLE_SANITIZE STREQUAL "")
        return()
    elseif (SUPPLE_SANITIZE STREQUAL "address")
        set(flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    elseif (SUPPLE_SANITIZE STREQUAL "thread")
        set(flags -fsanitize=thread)
    else()
        message(FATAL_ERROR "SUPPLE_SANITIZE must be empty, address or thread")
    endif()
    target_compile_options(${target} PRIVATE ${flags})
    target_link_options(${target} PRIVATE ${flags})
endfunction()

# Framework and product code: warnings on, symbols hidden. Hidden visibility
# is what lets every product use the same C++ names: nothing but the entry
# points leaves the binary, so two products in one host cannot bind to each
# other's code (on Linux the export maps below enforce the same thing).
function(supple_defaults target)
    set_target_properties(${target} PROPERTIES
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
        OBJC_VISIBILITY_PRESET hidden OBJCXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if (WIN32)
        # <windows.h> arrives through pugl's and GL's headers as well as our
        # own; without these it defines min and max as macros, which turns
        # every std::min and std::max after it into a syntax error.
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if (MSVC)
        # C4324: a struct padded to honour alignas -- the cache-line spacing
        # in Ring is that padding on purpose. C4127: a condition on a
        # compile-time constant (a product's kInstrument, kAnyRestart), which
        # is how the framework writes product-dependent code.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /wd4324 /wd4127)
        if (SUPPLE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Woverloaded-virtual>)
        if (SUPPLE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    supple_sanitize(${target})
endfunction()

include(${SUPPLE_FRAMEWORK}/cmake/Dependencies.cmake)

# --- generated identity -----------------------------------------------------------------

function(supple_c_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
supple_c_string(PRODUCT_NAME_C "${PRODUCT_NAME}")
supple_c_string(PRODUCT_DESCRIPTION_C "${PRODUCT_DESCRIPTION}")
supple_c_string(PRODUCT_VENDOR_C "${PRODUCT_VENDOR}")
supple_c_string(PRODUCT_URL_C "${PRODUCT_URL}")
if (PRODUCT_KIND STREQUAL "instrument")
    set(PRODUCT_IS_INSTRUMENT true)
    set(main_feature "instrument")
    set(au_type "aumu")
else()
    set(PRODUCT_IS_INSTRUMENT false)
    set(main_feature "audio-effect")
    set(au_type "aufx")
endif()
list(JOIN PRODUCT_CHANNELS ", " PRODUCT_CHANNELS_C)
set(PRODUCT_FEATURES_C "\"${main_feature}\"")
foreach(feature ${PRODUCT_FEATURES})
    string(APPEND PRODUCT_FEATURES_C ", \"${feature}\"")
endforeach()
configure_file(${SUPPLE_FRAMEWORK}/plugin/ProductInfo.h.in ${SUPPLE_GENERATED}/ProductInfo.h @ONLY)

function(supple_sources out)
    set(result "")
    foreach(source ${ARGN})
        list(APPEND result ${SUPPLE_ROOT}/${source})
    endforeach()
    set(${out} ${result} PARENT_SCOPE)
endfunction()
supple_sources(product_dsp_sources ${PRODUCT_DSP_SOURCES})
supple_sources(product_editor_sources ${PRODUCT_EDITOR_SOURCES})
supple_sources(product_test_sources ${PRODUCT_TEST_SOURCES})

# --- the product ----------------------------------------------------------------------------

# Everything SDK-free: parameters, the engine, the product's DSP.
add_library(product-core STATIC ${SUPPLE_FRAMEWORK}/core/Param.cpp ${product_dsp_sources})
target_include_directories(product-core PUBLIC ${SUPPLE_FRAMEWORK} ${SUPPLE_PRODUCT} ${SUPPLE_GENERATED})
supple_defaults(product-core)

if (SUPPLE_BUILD_GUI)
    function(supple_gui_library name metal)
        set(sources
            ${SUPPLE_FRAMEWORK}/gui/EditorBase.cpp ${SUPPLE_FRAMEWORK}/gui/Fonts.cpp
            ${SUPPLE_FRAMEWORK}/gui/Gui.cpp ${SUPPLE_FRAMEWORK}/gui/Ui.cpp
            ${SUPPLE_FRAMEWORK}/gui/Widgets.cpp ${SUPPLE_FRAMEWORK}/gui/Window.cpp
            ${product_editor_sources})
        if (metal)
            list(APPEND sources ${SUPPLE_FRAMEWORK}/gui/CanvasMetal.m)
            set_source_files_properties(${SUPPLE_FRAMEWORK}/gui/CanvasMetal.m PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
        else()
            list(APPEND sources ${SUPPLE_FRAMEWORK}/gui/CanvasGL.c)
        endif()
        add_library(${name} STATIC ${sources})
        target_link_libraries(${name} PUBLIC product-core supple-fonts)
        if (metal)
            target_link_libraries(${name} PUBLIC supple-nanovg-mtl supple-pugl)
            target_compile_definitions(${name} PUBLIC SUPPLE_METAL=1)
        else()
            target_link_libraries(${name} PUBLIC supple-nanovg $<IF:$<TARGET_EXISTS:supple-pugl-gl>,supple-pugl-gl,supple-pugl>)
            target_compile_definitions(${name} PUBLIC SUPPLE_METAL=0)
        endif()
        supple_defaults(${name})
    endfunction()
    supple_gui_library(product-gui ${SUPPLE_METAL})
    if (SUPPLE_METAL)
        # The preview tool reads back GL framebuffers; under Metal that would
        # be a different draw path from the one it exists to check.
        supple_gui_library(product-gui-gl OFF)
        set(SUPPLE_GL_GUI product-gui-gl)
    else()
        set(SUPPLE_GL_GUI product-gui)
    endif()
endif()

if (SUPPLE_BUILD_PLUGIN)
    add_library(product-impl STATIC ${SUPPLE_FRAMEWORK}/plugin/Plugin.cpp)
    target_link_libraries(product-impl PUBLIC product-core clap)
    if (SUPPLE_BUILD_GUI)
        target_link_libraries(product-impl PUBLIC product-gui)
        target_compile_definitions(product-impl PUBLIC SUPPLE_WITH_GUI=1)
    else()
        target_compile_definitions(product-impl PUBLIC SUPPLE_WITH_GUI=0)
    endif()
    supple_defaults(product-impl)

    set(formats CLAP VST3)
    if (APPLE)
        list(APPEND formats AUV2)
    endif()
    set(SUPPLE_ASSETS ${CMAKE_BINARY_DIR}/plugins)
    make_clapfirst_plugins(
        TARGET_NAME product-plugin
        IMPL_TARGET product-impl
        OUTPUT_NAME "${PRODUCT_NAME}"
        ENTRY_SOURCE "${SUPPLE_FRAMEWORK}/plugin/Entry.cpp"
        BUNDLE_IDENTIFIER "${PRODUCT_ID}"
        BUNDLE_VERSION "${PRODUCT_VERSION}"
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${formats}
        ASSET_OUTPUT_DIRECTORY ${SUPPLE_ASSETS}
        AUV2_MANUFACTURER_NAME "${PRODUCT_VENDOR}"
        AUV2_MANUFACTURER_CODE "${PRODUCT_AU_MANUFACTURER}"
        AUV2_SUBTYPE_CODE "${PRODUCT_AU_SUBTYPE}"
        AUV2_INSTRUMENT_TYPE "${au_type}")
    foreach(format clap vst3 auv2)
        if (TARGET product-plugin_${format})
            supple_sanitize(product-plugin_${format})
        endif()
    endforeach()

    # Linux modules export their entry points and nothing else. Anything more
    # is visible to every plugin in the process, and the GNU unique symbols
    # among it (fmt's locale facet id, the VST3 SDK's UpdateHandler) bind
    # across modules whatever RTLD_LOCAL says. -fvisibility cannot do this
    # alone: libstdc++ and pugl mark some of their own symbols default.
    if (UNIX AND NOT APPLE)
        foreach(format clap vst3)
            if (TARGET product-plugin_${format})
                set(map ${SUPPLE_FRAMEWORK}/cmake/exports-${format}.map)
                target_link_options(product-plugin_${format} PRIVATE "LINKER:--version-script=${map}")
                set_property(TARGET product-plugin_${format} APPEND PROPERTY LINK_DEPENDS ${map})
            endif()
        endforeach()
    endif()

    # The license and the third-party notices travel inside each bundle (or
    # beside the bare .clap/.vst3 files on Linux and Windows), because someone
    # handed only a binary never sees this repository. A target of its own,
    # always out of date, because a POST_BUILD on the plugins would only run
    # when they relink and editing the notices alone would ship the old copy.
    # The framework's notices, then any the product adds for its own
    # dependencies (product/NOTICES.txt). Composed at configure time; editing
    # either file reconfigures.
    set(notices ${SUPPLE_GENERATED}/NOTICES.txt)
    file(READ ${SUPPLE_FRAMEWORK}/NOTICES.txt notices_text)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SUPPLE_FRAMEWORK}/NOTICES.txt)
    # The glob, rechecked at every build, notices the file being created or
    # deleted; the configure dependency notices an edit.
    file(GLOB product_notices_file CONFIGURE_DEPENDS ${SUPPLE_PRODUCT}/NOTICES.txt)
    if (product_notices_file)
        file(READ ${SUPPLE_PRODUCT}/NOTICES.txt product_notices)
        string(APPEND notices_text "\n\n${product_notices}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SUPPLE_PRODUCT}/NOTICES.txt)
    endif()
    set(previous "")
    if (EXISTS ${notices})
        file(READ ${notices} previous)
    endif()
    if (NOT previous STREQUAL notices_text)  # unchanged text leaves the timestamp alone
        file(WRITE ${notices} "${notices_text}")
    endif()
    set(license ${SUPPLE_ROOT}/LICENSE)
    add_custom_target(product-notices ALL COMMENT "Staging the license and notices with the plugins")
    if (APPLE)
        foreach(spec "clap:${PRODUCT_NAME}.clap" "vst3:${PRODUCT_NAME}.vst3")
            string(REPLACE ":" ";" parts "${spec}")
            list(GET parts 0 format)
            list(GET parts 1 bundle)
            if (TARGET product-plugin_${format})
                add_dependencies(product-notices product-plugin_${format})
                add_custom_command(TARGET product-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${SUPPLE_ASSETS}/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${SUPPLE_ASSETS}/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${SUPPLE_ASSETS}/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        add_custom_command(TARGET product-notices POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SUPPLE_ASSETS}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${SUPPLE_ASSETS}/${PRODUCT_NAME}-NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${SUPPLE_ASSETS}/${PRODUCT_NAME}-LICENSE.txt"
            VERBATIM)
        # The Linux VST3 is a directory bundle, copied and installed whole:
        # its notices go inside it, not only beside it.
        if (NOT WIN32 AND TARGET product-plugin_vst3)
            set(resources "${SUPPLE_ASSETS}/${PRODUCT_NAME}.vst3/Contents/Resources")
            add_dependencies(product-notices product-plugin_vst3)
            add_custom_command(TARGET product-notices POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${resources}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${resources}/NOTICES.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${resources}/LICENSE.txt"
                VERBATIM)
        endif()
    endif()

    if (APPLE AND TARGET product-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper; an up-to-date AU binary does not rerun that copy, and
        # a component with CMake's plist is invisible to every host. Prepare
        # the bundle unconditionally, then sign it -- last, since adding a
        # file to a signed bundle invalidates the signature.
        set(component "${SUPPLE_ASSETS}/${PRODUCT_NAME}.component")
        add_custom_target(product-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/product-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${component}/Contents/Info.plist"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${component}/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${component}/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${component}/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none "${component}"
            DEPENDS product-plugin_auv2
            COMMENT "Preparing and ad-hoc signing ${PRODUCT_NAME}.component"
            VERBATIM)
    endif()

    # One target installs every format together, so a host never loads a
    # stale CLAP beside a fresh VST3. Ordinary builds never touch user plugin
    # folders.
    if (APPLE)
        set(install_specs
            "${PRODUCT_NAME}.clap|$ENV{HOME}/Library/Audio/Plug-Ins/CLAP|clap|dir"
            "${PRODUCT_NAME}.vst3|$ENV{HOME}/Library/Audio/Plug-Ins/VST3|vst3|dir"
            "${PRODUCT_NAME}.component|$ENV{HOME}/Library/Audio/Plug-Ins/Components|auv2|dir")
    elseif (WIN32)
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" local_app_data)
        set(install_specs
            "${PRODUCT_NAME}.clap|${local_app_data}/Programs/Common/CLAP|clap|file"
            "${PRODUCT_NAME}.vst3|${local_app_data}/Programs/Common/VST3|vst3|file")
    else()
        set(install_specs
            "${PRODUCT_NAME}.clap|$ENV{HOME}/.clap|clap|file"
            "${PRODUCT_NAME}.vst3|$ENV{HOME}/.vst3|vst3|dir")
    endif()
    add_custom_target(product-install COMMENT "Installing every ${PRODUCT_NAME} format")
    add_dependencies(product-install product-notices)
    if (TARGET product-prepare-auv2)
        add_dependencies(product-install product-prepare-auv2)
    endif()
    foreach(spec ${install_specs})
        string(REPLACE "|" ";" parts "${spec}")
        list(GET parts 0 artifact)
        list(GET parts 1 destination)
        list(GET parts 2 format)
        list(GET parts 3 kind)
        if (NOT TARGET product-plugin_${format})
            continue()
        endif()
        add_dependencies(product-install product-plugin_${format})
        if (kind STREQUAL "dir")
            set(source "${SUPPLE_ASSETS}/${artifact}")
            set(copy copy_directory)
        else()
            set(source "$<TARGET_FILE:product-plugin_${format}>")
            set(copy copy)
        endif()
        add_custom_command(TARGET product-install POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${artifact}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
            COMMAND ${CMAKE_COMMAND} -E ${copy} "${source}" "${destination}/${artifact}"
            VERBATIM)
        if (NOT APPLE)
            add_custom_command(TARGET product-install POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${destination}/${PRODUCT_NAME}-NOTICES.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${destination}/${PRODUCT_NAME}-LICENSE.txt"
                VERBATIM)
        endif()
    endforeach()
    if (APPLE)
        add_custom_command(TARGET product-install POST_BUILD
            COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true" VERBATIM)
    endif()
endif()

if (SUPPLE_BUILD_TESTS)
    include(${SUPPLE_FRAMEWORK}/cmake/Tests.cmake)
endif()
