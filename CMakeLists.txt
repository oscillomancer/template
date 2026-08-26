cmake_minimum_required(VERSION 3.21)

# This must be set before project(): project() creates the cache entry, and a
# later non-FORCE assignment leaves it empty. macOS 11 is the first release that
# can run the arm64 slice produced by current Apple Silicon builds.
if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

if (APPLE)
    project(starter VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(starter VERSION 0.1.0 LANGUAGES C CXX)
endif()

# A bare `cmake -S . -B build` sets no build type on single-config generators,
# which silently produces an unoptimised plugin. The presets always name one;
# this is for everybody who does not use them.
get_property(_starter_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _starter_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()



set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(STARTER_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(STARTER_BUILD_TOOLS "Build tests and integration probes" ON)
option(STARTER_BUILD_GUI "Build the Pugl + NanoVG editor" ON)
option(STARTER_STRICT_WARNINGS "Enable warnings on starter-owned code" ON)
option(STARTER_SANITIZERS "Enable AddressSanitizer and UBSan on starter-owned code" OFF)

function(starter_target_defaults target)
    if (STARTER_STRICT_WARNINGS)
        if (MSVC)
            target_compile_options(${target} PRIVATE /W4)
        else()
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic)
            if (APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                # NanoVG's public color type intentionally uses this C extension.
                target_compile_options(${target} PRIVATE
                    -Wno-gnu-anonymous-struct -Wno-nested-anon-types)
            endif()
        endif()
    endif()
    if (STARTER_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(starter-dsp INTERFACE)
target_include_directories(starter-dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)

if (STARTER_BUILD_TOOLS)
    enable_testing()
    add_executable(starter-cli tools/starter_cli.cpp)
    target_link_libraries(starter-cli PRIVATE starter-dsp)
    starter_target_defaults(starter-cli)
    add_executable(starter-test test/test_core.cpp)
    target_link_libraries(starter-test PRIVATE starter-dsp)
    starter_target_defaults(starter-test)
    add_test(NAME core COMMAND starter-test)
endif()

function(starter_add_gui_preview)
    if (TARGET starter-gui-gl)
        set(preview_gui starter-gui-gl)
    else()
        set(preview_gui starter-gui)
    endif()
    add_executable(starter-gui-preview tools/gui_preview.cpp)
    target_link_libraries(starter-gui-preview PRIVATE ${preview_gui})
    target_include_directories(starter-gui-preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
endfunction()

if (STARTER_BUILD_PLUGIN)
    include(FetchContent)
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)
    set(CLAP_WRAPPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(clap-wrapper
        GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
        GIT_TAG 1cca996e96f29ab2be7ae9f8cfe532bbc92e1dd6
        GIT_SHALLOW OFF)
    FetchContent_MakeAvailable(clap-wrapper)

    # clap-wrapper 0.16.0 keeps only the final VST3 automation point in each
    # parameter queue. Apply the small upstreamable fix after population; the
    # reverse check makes repeated configure runs idempotent.
    set(starter_wrapper_vst3_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-all-points.patch)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --check ${starter_wrapper_vst3_patch}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
        RESULT_VARIABLE wrapper_patch_applicable
        OUTPUT_QUIET ERROR_QUIET)
    if (wrapper_patch_applicable EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply ${starter_wrapper_vst3_patch}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${starter_wrapper_vst3_patch}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            RESULT_VARIABLE wrapper_patch_present
            OUTPUT_QUIET ERROR_QUIET)
        if (NOT wrapper_patch_present EQUAL 0)
            message(FATAL_ERROR "clap-wrapper VST3 automation patch no longer applies")
        endif()
    endif()

    # register_timer() clamps every request to 30ms upstream, capping a VST3
    # editor at 33fps however fast it asked to run. The AUv2 path returns false
    # instead and falls through to the plugin's own timer at the full 60Hz,
    # which is why an AU looks smoother than the VST3 of the same build.
    set(starter_wrapper_vst3_patch_timer ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-timer-floor.patch)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --check ${starter_wrapper_vst3_patch_timer}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
        RESULT_VARIABLE wrapper_timer_applicable
        OUTPUT_QUIET ERROR_QUIET)
    if (wrapper_timer_applicable EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply ${starter_wrapper_vst3_patch_timer}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${starter_wrapper_vst3_patch_timer}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            RESULT_VARIABLE wrapper_timer_present
            OUTPUT_QUIET ERROR_QUIET)
        if (NOT wrapper_timer_present EQUAL 0)
            message(FATAL_ERROR "clap-wrapper VST3 timer floor patch no longer applies")
        endif()
    endif()

    if (STARTER_BUILD_TOOLS)
        add_test(NAME wrapper-automation
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/detail/vst3/process.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_automation.cmake)
        add_test(NAME wrapper-timer
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/wrapasvst3.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_timer.cmake)
    endif()

    add_library(starter-impl STATIC plugin/src/starter_clap.cpp)
    target_link_libraries(starter-impl PUBLIC clap clap-wrapper-extensions starter-dsp)
    starter_target_defaults(starter-impl)

    if (STARTER_BUILD_GUI)
        add_subdirectory(plugin/gui)
        target_link_libraries(starter-impl PUBLIC starter-gui)
        target_compile_definitions(starter-impl PUBLIC STARTER_WITH_GUI=1)
        if (STARTER_BUILD_TOOLS)
            starter_add_gui_preview()
            add_executable(starter-editor-test test/editor_input.cpp)
            target_link_libraries(starter-editor-test PRIVATE
                $<IF:$<TARGET_EXISTS:starter-gui-gl>,starter-gui-gl,starter-gui>)
            target_include_directories(starter-editor-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            add_test(NAME editor-input COMMAND starter-editor-test)

            add_executable(starter-clap-probe tools/clap_host_probe.cpp)
            target_link_libraries(starter-clap-probe PRIVATE clap)
            target_include_directories(starter-clap-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            if (APPLE)
                set_source_files_properties(tools/clap_host_probe.cpp PROPERTIES LANGUAGE OBJCXX)
                target_compile_options(starter-clap-probe PRIVATE -fno-objc-arc)
                target_link_libraries(starter-clap-probe PRIVATE "-framework Cocoa")
            endif()
        endif()
    else()
        target_compile_definitions(starter-impl PUBLIC STARTER_WITH_GUI=0)
    endif()

    # A tiny host that dlopens the built .clap and drives it the way a DAW
    # would. The CLAP is what clap-wrapper turns into the VST3 and the AU, so
    # exercising it covers the shared code path in all three formats.
    if (STARTER_BUILD_TOOLS)
        add_executable(starter-host-smoke test/host_smoke.cpp)
        target_link_libraries(starter-host-smoke PRIVATE clap)
        target_compile_definitions(starter-host-smoke PRIVATE
            HOST_SMOKE_PLUGIN_ID="com.example.starter"
            HOST_SMOKE_PLAIN_PARAM="Input Gain")
        starter_target_defaults(starter-host-smoke)
        add_test(NAME host-smoke
                 COMMAND starter-host-smoke
                         ${CMAKE_BINARY_DIR}/starter_assets/Starter.clap)
    endif()

    set(STARTER_FORMATS CLAP VST3)
    if (APPLE)
        list(APPEND STARTER_FORMATS AUV2)
    endif()

    # CUSTOMIZE BEFORE SHIPPING: every product needs permanent unique IDs,
    # vendor text and AU four-character codes.
    make_clapfirst_plugins(
        TARGET_NAME starter-plugin
        IMPL_TARGET starter-impl
        OUTPUT_NAME "Starter"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/src/starter_entry.cpp"
        BUNDLE_IDENTIFIER "com.example.starter"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${STARTER_FORMATS}
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/starter_assets
        AUV2_MANUFACTURER_NAME "Your Company"
        AUV2_MANUFACTURER_CODE "StCo"
        AUV2_SUBTYPE_CODE "StFx"
        AUV2_INSTRUMENT_TYPE "aufx")

    # Attribution has to travel with the binary. NOTICES.txt sitting in the
    # repository does nothing for someone who is handed only a bundle, and every
    # dependency in the stack requires its notice to accompany what is
    # distributed. The install target copies whole bundles, so anything placed
    # inside one is carried along for free.
    #
    # This is a target of its own rather than a POST_BUILD on each plugin: a
    # POST_BUILD only runs when its target relinks, so editing NOTICES.txt alone
    # would leave the bundles holding the previous copy. That is the same
    # staleness that already bit the AU's Info.plist, and a custom target is
    # always out of date, so the copy is reconsidered every build.
    set(starter_notices "${CMAKE_CURRENT_SOURCE_DIR}/NOTICES.txt")

    add_custom_target(starter-notices ALL
        COMMENT "Staging third-party notices with the built plugins")

    if (APPLE)
        # All three macOS formats are bundles, so the notices go where a bundle
        # is supposed to keep them. The AU is absent here on purpose: its copy
        # has to precede the code signature, so it lives in the prepare step
        # below instead.
        foreach(spec "starter-plugin_clap:Starter.clap" "starter-plugin_vst3:Starter.vst3")
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 target)
            list(GET parts 1 bundle)
            if (TARGET ${target})
                add_dependencies(starter-notices ${target})
                add_custom_command(TARGET starter-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${CMAKE_BINARY_DIR}/starter_assets/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${starter_notices}"
                        "${CMAKE_BINARY_DIR}/starter_assets/${bundle}/Contents/Resources/NOTICES.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        # A CLAP on Linux or Windows is a single shared library with no bundle
        # to put anything in, so the file lands beside the built artifacts and
        # it is the release archive that has to carry it.
        add_custom_command(TARGET starter-notices POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${starter_notices}"
                "${CMAKE_BINARY_DIR}/starter_assets/NOTICES.txt"
            VERBATIM)
    endif()

    if (APPLE AND TARGET starter-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper. If the AU binary is already up to date, its post-build
        # copy does not rerun and an install can otherwise copy an unregistered
        # component. Prepare it unconditionally before aggregate builds/install.
        add_custom_target(starter-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/starter-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${CMAKE_BINARY_DIR}/starter_assets/Starter.component/Contents/Info.plist"
            # Before the signature, not after: adding a file to a signed bundle
            # invalidates it, and the auv2-bundle test checks the signature.
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/starter_assets/Starter.component/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${starter_notices}"
                "${CMAKE_BINARY_DIR}/starter_assets/Starter.component/Contents/Resources/NOTICES.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none
                "${CMAKE_BINARY_DIR}/starter_assets/Starter.component"
            DEPENDS starter-plugin_auv2
            COMMENT "Preparing and ad-hoc signing Starter.component"
            VERBATIM)
        if (STARTER_BUILD_TOOLS)
            add_test(NAME auv2-bundle
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=${CMAKE_BINARY_DIR}/starter_assets/Starter.component
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_auv2_bundle.cmake)
        endif()
    endif()

    if (APPLE)
        set(STARTER_INSTALL_SPECS
            "Starter.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:starter-plugin_clap:dir"
            "Starter.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:starter-plugin_vst3:dir"
            "Starter.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:starter-plugin_auv2:dir")
    elseif (UNIX)
        # On Linux a CLAP is a bare shared object, not a bundle directory --
        # only the VST3 is a folder here -- so the two are copied differently.
        set(STARTER_INSTALL_SPECS
            "Starter.clap:$ENV{HOME}/.clap:starter-plugin_clap:file"
            "Starter.vst3:$ENV{HOME}/.vst3:starter-plugin_vst3:dir")
    endif()

    if (STARTER_INSTALL_SPECS)
        add_custom_target(starter-install COMMENT "Installing every Starter format")
        if (TARGET starter-prepare-auv2)
            add_dependencies(starter-install starter-prepare-auv2)
        endif()
        foreach(spec ${STARTER_INSTALL_SPECS})
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 bundle)
            list(GET parts 1 destination)
            list(GET parts 2 target)
            list(GET parts 3 kind)
            if (kind STREQUAL "dir")
                set(copyCmd copy_directory)
            else()
                set(copyCmd copy)
            endif()
            if (TARGET ${target})
                add_dependencies(starter-install ${target})
                add_custom_command(TARGET starter-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E ${copyCmd}
                        "${CMAKE_BINARY_DIR}/starter_assets/${bundle}"
                        "${destination}/${bundle}"
                    VERBATIM)
            endif()
        endforeach()
        if (APPLE)
            add_custom_command(TARGET starter-install POST_BUILD
                COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true"
                VERBATIM)
        endif()
    endif()
endif()
