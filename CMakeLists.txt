cmake_minimum_required(VERSION 3.21)

if (APPLE)
    project(starter VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(starter VERSION 0.1.0 LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(STARTER_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(STARTER_BUILD_TOOLS "Build tests and integration probes" ON)
option(STARTER_BUILD_GUI "Build the Pugl + NanoVG editor" ON)

add_library(starter-dsp INTERFACE)
target_include_directories(starter-dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)

if (STARTER_BUILD_TOOLS)
    enable_testing()
    add_executable(starter-cli tools/starter_cli.cpp)
    target_link_libraries(starter-cli PRIVATE starter-dsp)
    add_executable(starter-test test/test_core.cpp)
    target_link_libraries(starter-test PRIVATE starter-dsp)
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

    add_library(starter-impl STATIC plugin/src/starter_clap.cpp)
    target_link_libraries(starter-impl PUBLIC clap clap-wrapper-extensions starter-dsp)

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

    if (APPLE)
        set(STARTER_INSTALL_SPECS
            "Starter.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:starter-plugin_clap"
            "Starter.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:starter-plugin_vst3"
            "Starter.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:starter-plugin_auv2")
    elseif (UNIX)
        set(STARTER_INSTALL_SPECS
            "Starter.clap:$ENV{HOME}/.clap:starter-plugin_clap"
            "Starter.vst3:$ENV{HOME}/.vst3:starter-plugin_vst3")
    endif()

    if (STARTER_INSTALL_SPECS)
        add_custom_target(starter-install COMMENT "Installing every Starter format")
        foreach(spec ${STARTER_INSTALL_SPECS})
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 bundle)
            list(GET parts 1 destination)
            list(GET parts 2 target)
            if (TARGET ${target})
                add_dependencies(starter-install ${target})
                add_custom_command(TARGET starter-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
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
