# Tests, tools and probes. See BUILDING.md for what each one proves.

enable_testing()
find_package(Threads REQUIRED)

function(supple_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LINK" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${T_LINK} Threads::Threads)
    target_include_directories(${name} PRIVATE ${SUPPLE_FRAMEWORK}/test)
    supple_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# --- SDK-free -----------------------------------------------------------------------------

add_test(NAME framework-update
    COMMAND ${CMAKE_COMMAND} -DFRAMEWORK=${SUPPLE_FRAMEWORK}
        -DWORK=${CMAKE_BINARY_DIR}/framework-update-test
        -P ${SUPPLE_FRAMEWORK}/test/update_framework_test.cmake)
if (UNIX AND EXISTS "${SUPPLE_ROOT}/.github/scripts/clap-probe-mac.sh")
    add_test(NAME ci-probe-checker COMMAND bash ${SUPPLE_FRAMEWORK}/test/ci_probe_test.sh
        ${SUPPLE_ROOT}/.github/scripts/clap-probe-mac.sh)
endif()

supple_test(core-tests SOURCES ${SUPPLE_FRAMEWORK}/test/core_test.cpp LINK product-core)
if (product_test_sources)
    supple_test(processor-tests SOURCES ${product_test_sources} LINK product-core)
endif()

add_executable(render-cli ${SUPPLE_FRAMEWORK}/tools/render_cli.cpp)
target_link_libraries(render-cli PRIVATE product-core)
supple_defaults(render-cli)

# --- editor -------------------------------------------------------------------------------

if (SUPPLE_BUILD_GUI)
    supple_test(ui-tests SOURCES ${SUPPLE_FRAMEWORK}/test/ui_test.cpp LINK ${SUPPLE_GL_GUI})

    add_executable(gui-preview ${SUPPLE_FRAMEWORK}/tools/gui_preview.cpp)
    target_link_libraries(gui-preview PRIVATE ${SUPPLE_GL_GUI})
    supple_defaults(gui-preview)
endif()

# --- plugin -------------------------------------------------------------------------------

if (SUPPLE_BUILD_PLUGIN)
    supple_test(probe-audio-tests SOURCES ${SUPPLE_FRAMEWORK}/test/probe_audio_test.cpp LINK product-core clap)
    # The CLAP surface, in process: a scripted host with real threads.
    supple_test(plugin-tests SOURCES ${SUPPLE_FRAMEWORK}/test/plugin_test.cpp LINK product-impl)

    # A host that loads the built .clap from disk, once per bus layout.
    add_executable(host-smoke ${SUPPLE_FRAMEWORK}/test/host_smoke.cpp)
    target_link_libraries(host-smoke PRIVATE clap ${CMAKE_DL_LIBS})
    target_compile_definitions(host-smoke PRIVATE
        HOST_SMOKE_PLUGIN_ID="${PRODUCT_ID}"
        HOST_SMOKE_PLAIN_PARAM="${PRODUCT_SMOKE_PLAIN_PARAM}")
    if (PRODUCT_SMOKE_LATENCY_PARAM)
        target_compile_definitions(host-smoke PRIVATE HOST_SMOKE_LATENCY_PARAM="${PRODUCT_SMOKE_LATENCY_PARAM}")
    endif()
    supple_defaults(host-smoke)
    # Once per layout (cmake/Capabilities.cmake): host-smoke-2ch, -5.1, -ambi1,
    # or -notes for a note effect.
    foreach(layout ${SUPPLE_LAYOUTS})
        if (layout MATCHES "^[0-9]+$")
            set(name ${layout}ch)
        elseif (layout STREQUAL "none")
            set(name notes)
        else()
            set(name ${layout})
        endif()
        add_test(NAME host-smoke-${name}
            COMMAND host-smoke $<TARGET_FILE:product-plugin_clap> --layout ${layout})
    endforeach()

    # Each wrapper patch, still applied to the pinned checkout.
    get_property(patches GLOBAL PROPERTY SUPPLE_PATCHES)
    foreach(entry ${patches})
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 dir)
        list(GET parts 2 file)
        add_test(NAME patch-${name} COMMAND ${GIT_EXECUTABLE} -C "${dir}" apply --reverse --check "${file}")
    endforeach()

    # A minimal GUI host: macOS window embedding, timers, resize, teardown.
    add_executable(clap-probe ${SUPPLE_FRAMEWORK}/tools/clap_probe.cpp)
    target_link_libraries(clap-probe PRIVATE clap ${CMAKE_DL_LIBS})
    supple_defaults(clap-probe)
    if (APPLE)
        set_source_files_properties(${SUPPLE_FRAMEWORK}/tools/clap_probe.cpp PROPERTIES LANGUAGE OBJCXX)
        target_compile_options(clap-probe PRIVATE -fno-objc-arc)
        target_link_libraries(clap-probe PRIVATE "-framework Cocoa")
    endif()

    if (UNIX AND NOT APPLE)
        foreach(spec "clap:clap_entry" "vst3:GetPluginFactory,ModuleEntry,ModuleExit,clap_entry")
            string(REPLACE ":" ";" parts "${spec}")
            list(GET parts 0 format)
            list(GET parts 1 expected)
            add_test(NAME exports-${format}
                COMMAND ${CMAKE_COMMAND} -DMODULE=$<TARGET_FILE:product-plugin_${format}> "-DEXPECTED=${expected}"
                        -P ${SUPPLE_FRAMEWORK}/cmake/CheckExports.cmake)
        endforeach()
        add_executable(vst3-unload-probe ${SUPPLE_FRAMEWORK}/test/vst3_unload_probe.cpp)
        target_link_libraries(vst3-unload-probe PRIVATE ${CMAKE_DL_LIBS})
        supple_defaults(vst3-unload-probe)
        add_test(NAME vst3-unload COMMAND vst3-unload-probe $<TARGET_FILE:product-plugin_vst3>)

        # The VST3 against the CLAP it wraps: buses, layouts, MIDI mapping, and
        # the same performance rendered through both. A host needs the SDK's
        # interface IDs and nothing else of it.
        set(vst3_sdk "${CPM_PACKAGE_vst3sdk_SOURCE_DIR}")
        if (vst3_sdk AND EXISTS "${vst3_sdk}/pluginterfaces/base/funknown.cpp")
            add_library(supple-vst3-iids OBJECT ${vst3_sdk}/pluginterfaces/base/funknown.cpp
                ${vst3_sdk}/pluginterfaces/base/coreiids.cpp ${vst3_sdk}/public.sdk/source/vst/vstinitiids.cpp)
            target_include_directories(supple-vst3-iids SYSTEM PRIVATE ${vst3_sdk})
            target_compile_options(supple-vst3-iids PRIVATE -w)
            supple_sanitize(supple-vst3-iids)
            add_executable(vst3-host-tests ${SUPPLE_FRAMEWORK}/test/vst3_host_test.cpp $<TARGET_OBJECTS:supple-vst3-iids>)
            target_include_directories(vst3-host-tests SYSTEM PRIVATE ${vst3_sdk})
            target_include_directories(vst3-host-tests PRIVATE ${SUPPLE_FRAMEWORK}/test)
            target_link_libraries(vst3-host-tests PRIVATE product-core clap ${CMAKE_DL_LIBS})
            target_compile_definitions(vst3-host-tests PRIVATE VST3_HOST_PLAIN_PARAM="${PRODUCT_SMOKE_PLAIN_PARAM}")
            supple_defaults(vst3-host-tests)
            # The SDK's interfaces have no virtual destructors, by design.
            target_compile_options(vst3-host-tests PRIVATE -Wno-non-virtual-dtor)
            add_test(NAME vst3-host COMMAND vst3-host-tests $<TARGET_FILE:product-plugin_vst3>
                $<TARGET_FILE:product-plugin_clap>)
        endif()

        if (SUPPLE_BUILD_GUI)
            # A real X11 parent: embedding, pixels, resize, scale, reopen,
            # teardown while audio runs. Skipped (77) without a display.
            find_package(X11 REQUIRED)
            add_executable(x11-probe ${SUPPLE_FRAMEWORK}/tools/x11_probe.cpp)
            target_link_libraries(x11-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            supple_defaults(x11-probe)
            add_test(NAME x11-embedding
                COMMAND x11-probe $<TARGET_FILE:product-plugin_clap> ${CMAKE_BINARY_DIR}/x11-probe.ppm)
            set_tests_properties(x11-embedding PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
        endif()
    endif()

    if (WIN32 AND SUPPLE_BUILD_GUI)
        # A real Win32 parent: embedding, pixels, resize, set_scale(2),
        # hide/show, three create/destroy cycles while audio runs. Needs an
        # OpenGL 2 driver; see the note at the top of win32_probe.cpp.
        add_executable(win32-probe ${SUPPLE_FRAMEWORK}/tools/win32_probe.cpp)
        target_link_libraries(win32-probe PRIVATE clap user32 gdi32 Threads::Threads)
        target_compile_definitions(win32-probe PRIVATE PROBE_PLAIN_PARAM="${PRODUCT_SMOKE_PLAIN_PARAM}")
        supple_defaults(win32-probe)
        add_test(NAME win32-embedding
            COMMAND win32-probe $<TARGET_FILE:product-plugin_clap> ${CMAKE_BINARY_DIR}/win32-probe.ppm)
        # A host without clap.timer-support: the editor's own WM_TIMER
        # fallback must carry it through the same lifecycle.
        add_test(NAME win32-embedding-no-host-timer
            COMMAND win32-probe $<TARGET_FILE:product-plugin_clap> ${CMAKE_BINARY_DIR}/win32-probe-no-timer.ppm
                    --no-host-timer)
        set_tests_properties(win32-embedding win32-embedding-no-host-timer PROPERTIES RUN_SERIAL TRUE)
        if (TARGET product-plugin_vst3)
            add_executable(win32-module-test ${SUPPLE_FRAMEWORK}/test/win32_module_test.cpp)
            target_link_libraries(win32-module-test PRIVATE clap user32)
            supple_defaults(win32-module-test)
            add_test(NAME win32-module-ownership COMMAND win32-module-test
                $<TARGET_FILE:product-plugin_clap> $<TARGET_FILE:product-plugin_vst3>)
            set_tests_properties(win32-module-ownership PROPERTIES RUN_SERIAL TRUE)
        endif()
    endif()

    if (APPLE AND TARGET product-plugin_auv2)
        add_test(NAME auv2-bundle
            COMMAND ${CMAKE_COMMAND} -DBUNDLE=${SUPPLE_ASSETS}/${PRODUCT_NAME}.component
                    -P ${SUPPLE_FRAMEWORK}/cmake/CheckAuBundle.cmake)
    endif()

    # free-audio/clap-validator, when installed (cargo install --git
    # https://github.com/free-audio/clap-validator), or pointed to with
    # -DSUPPLE_CLAP_VALIDATOR=/path/to/clap-validator.
    find_program(SUPPLE_CLAP_VALIDATOR clap-validator)
    if (SUPPLE_CLAP_VALIDATOR AND NOT SUPPLE_SANITIZE STREQUAL "")
        # It cannot load a module that expects a sanitizer runtime from its host.
        message(STATUS "clap-validator is not run against a sanitizer build")
    elseif (SUPPLE_CLAP_VALIDATOR)
        if (APPLE)
            set(validated "${SUPPLE_ASSETS}/${PRODUCT_NAME}.clap")
        else()
            set(validated "$<TARGET_FILE:product-plugin_clap>")
        endif()
        set(validator_options --only-failed)
        if (PRODUCT_PRESETS)
            # clap-validator 0.4.1 deadlocks on the second preset of any
            # container -- a plugin location listing two factory presets, as
            # CLAP has them -- in its own metadata receiver (begin_preset holds
            # the lock flush_preset takes). plugin-tests crawls and loads them
            # instead. Drop this when a release fixes it.
            list(APPEND validator_options --exclude "^preset-discovery-(crawl|load)$")
        endif()
        add_test(NAME clap-validator COMMAND ${SUPPLE_CLAP_VALIDATOR} validate ${validator_options} "${validated}")
        set_tests_properties(clap-validator PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "clap-validator not found; its test is not registered")
    endif()
endif()
