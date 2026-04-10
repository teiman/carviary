cmake_minimum_required(VERSION 3.10)
project(carviary CXX)

file(GLOB SOURCES ${CMAKE_SOURCE_DIR}/*.cpp)

add_executable(carviary WIN32 ${SOURCES} ${CMAKE_SOURCE_DIR}/winquake.rc)

target_include_directories(carviary PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/headers)
target_compile_definitions(carviary PRIVATE WIN32 _WINDOWS)

# Allow C-isms during incremental port to C++
target_compile_options(carviary PRIVATE /permissive /Zc:strictStrings- /W3 /wd4090)

set_property(TARGET carviary PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

target_link_options(carviary PRIVATE
    /NODEFAULTLIB:LIBC.lib
    /NODEFAULTLIB:LIBCD.lib
    /NODEFAULTLIB:libcmt.lib
    /SUBSYSTEM:WINDOWS
)

target_link_libraries(carviary PRIVATE
    legacy_stdio_definitions.lib
    oldnames.lib
    comctl32 dxguid gdi32 kernel32 opengl32 user32 winmm wsock32 dsound
)

set_target_properties(carviary PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "d:/quake"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "d:/quake"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "d:/quake"
    OUTPUT_NAME "carviary"
)
