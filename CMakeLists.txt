cmake_minimum_required(VERSION 3.21)
project(M602Saver VERSION 1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no VC++ runtime DLL needed
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")

include(cmake/embed_font.cmake)
m602_generate_font("${CMAKE_CURRENT_BINARY_DIR}/font_data.cpp")

# Simulation and renderer, shared by the screensavers and the tests.
add_library(m602core STATIC
    src/saver.cpp
    src/renderer.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/font_data.cpp")
target_include_directories(m602core PUBLIC src)
if(MSVC)
    target_compile_options(m602core PUBLIC /W4 /permissive- /utf-8)
    # Draw declares its own I that hides ScreenSaver's, as in the Pascal.
    set_source_files_properties(src/saver.cpp PROPERTIES COMPILE_OPTIONS /wd4458)
else()
    target_compile_options(m602core PUBLIC -Wall -Wextra)
endif()

if(WIN32)
    enable_language(RC)
    add_executable(M602Saver WIN32 src/winmain.cpp src/saver.rc)
    target_link_libraries(M602Saver PRIVATE m602core comctl32 comdlg32)
    set_target_properties(M602Saver PROPERTIES SUFFIX ".scr")
    target_compile_definitions(M602Saver PRIVATE UNICODE _UNICODE)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/version.h" CONTENT
        "#define M602_VERSION_MAJOR ${PROJECT_VERSION_MAJOR}\n#define M602_VERSION_MINOR ${PROJECT_VERSION_MINOR}\n#define M602_VERSION_STRING \"${PROJECT_VERSION}\"\n")
    target_include_directories(M602Saver PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
elseif(APPLE)
    enable_language(OBJCXX)
    add_library(M602Saver MODULE src/mac/M602SaverView.mm)
    target_link_libraries(M602Saver PRIVATE m602core "-framework Cocoa" "-framework ScreenSaver")
    target_compile_options(M602Saver PRIVATE -fobjc-arc)
    set_target_properties(M602Saver PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION saver
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/mac/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER com.missingno7.M602Saver
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})

    # Loads the built bundle, renders frames and the settings sheet to PNG.
    add_executable(mac_smoke tests/mac_smoke.mm)
    target_link_libraries(mac_smoke PRIVATE "-framework Cocoa" "-framework ScreenSaver")
    target_compile_options(mac_smoke PRIVATE -fobjc-arc)
endif()

enable_testing()
add_executable(saver_tests tests/saver_tests.cpp)
target_link_libraries(saver_tests PRIVATE m602core)
target_compile_definitions(saver_tests PRIVATE
    TRACE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/dosref")
if(MSVC)
    target_compile_definitions(saver_tests PRIVATE _CRT_SECURE_NO_WARNINGS)  # getenv
endif()
add_test(NAME saver_tests COMMAND saver_tests)
