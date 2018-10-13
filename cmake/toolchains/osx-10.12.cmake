find_program(XCRUN_EXECUTABLE xcrun)
find_program(XCODE_SELECT_EXECUTABLE xcode-select)

execute_process(
    COMMAND ${XCRUN_EXECUTABLE} --find "clang"
    OUTPUT_VARIABLE XCODE_COMPILER_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND ${XCRUN_EXECUTABLE} --show-sdk-version
    OUTPUT_VARIABLE OSX_SDK_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND ${XCODE_SELECT_EXECUTABLE} -print-path
    OUTPUT_VARIABLE XCODE_DEVELOPER_ROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(CMAKE_XCODE_ATTRIBUTE_CC "${XCODE_COMPILER_PATH}" CACHE STRING "Xcode Compiler" FORCE)
set(CMAKE_OSX_SYSROOT "${XCODE_DEVELOPER_ROOT}/Platforms/MacOSX.platform/Developer/SDKs/MacOSX${OSX_SDK_VERSION}.sdk" CACHE STRING "System root for OSX" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.12" CACHE STRING "OS X Deployment target" FORCE)
set(CMAKE_CXX_STANDARD 11 CACHE STRING "C++ Standard (toolchain)" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED YES CACHE BOOL "C++ Standard required" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "C++ Position Independent Code is required" FORCE)

