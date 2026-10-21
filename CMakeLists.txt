cmake_minimum_required(VERSION 3.21)
project(M602Saver VERSION 1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no VC++ runtime DLL needed

include(cmake/embed_font.cmake)
m602_generate_font("${CMAKE_CURRENT_BINARY_DIR}/font_data.cpp")

# Simulation and renderer, shared by the screensaver and the tests.
add_library(m602core STATIC
    src/saver.cpp
    src/renderer.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/font_data.cpp")
target_include_directories(m602core PUBLIC src)
if(MSVC)
    target_compile_options(m602core PUBLIC /W4 /permissive- /utf-8)
endif()

add_executable(M602Saver WIN32 src/winmain.cpp src/saver.rc)
target_link_libraries(M602Saver PRIVATE m602core comctl32 comdlg32)
set_target_properties(M602Saver PROPERTIES SUFFIX ".scr")
target_compile_definitions(M602Saver PRIVATE UNICODE _UNICODE)

enable_testing()
add_executable(saver_tests tests/saver_tests.cpp)
target_link_libraries(saver_tests PRIVATE m602core)
target_compile_definitions(saver_tests PRIVATE
    TRACE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/dosref")
add_test(NAME saver_tests COMMAND saver_tests)
