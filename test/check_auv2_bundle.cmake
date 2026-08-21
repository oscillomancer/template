if (NOT DEFINED BUNDLE OR NOT EXISTS "${BUNDLE}/Contents/Info.plist")
    message(FATAL_ERROR "AUv2 bundle not found: ${BUNDLE}")
endif()

file(READ "${BUNDLE}/Contents/Info.plist" plist)
string(FIND "${plist}" "<key>AudioComponents</key>" components)
if (components EQUAL -1)
    message(FATAL_ERROR "AUv2 bundle has CMake's generic plist, not clap-wrapper's AudioComponents plist")
endif()

execute_process(
    COMMAND /usr/bin/codesign --verify --deep --strict "${BUNDLE}"
    RESULT_VARIABLE signature_result
    ERROR_VARIABLE signature_error)
if (NOT signature_result EQUAL 0)
    message(FATAL_ERROR "AUv2 bundle signature is invalid: ${signature_error}")
endif()
