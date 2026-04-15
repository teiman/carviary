cmake_minimum_required(VERSION 3.16)
project(carviary_sdl3 CXX)

# Find SDL3
find_package(SDL3 REQUIRED)

# Find OpenGL
find_package(OpenGL REQUIRED)

file(GLOB SOURCES ${CMAKE_SOURCE_DIR}/*.cpp)

add_executable(carviary ${SOURCES})

target_include_directories(carviary PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/headers
)

target_compile_definitions(carviary PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32
    _WINDOWS
)

if(MSVC)
    target_compile_options(carviary PRIVATE /permissive /Zc:strictStrings- /W3 /wd4090 /wd4244)
    set_property(TARGET carviary PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    target_link_options(carviary PRIVATE
        /NODEFAULTLIB:LIBC.lib
        /NODEFAULTLIB:LIBCD.lib
        /NODEFAULTLIB:libcmt.lib
        /SUBSYSTEM:CONSOLE
        /STACK:4194304
    )
    target_link_libraries(carviary PRIVATE legacy_stdio_definitions.lib oldnames.lib)
endif()

target_link_libraries(carviary PRIVATE
    SDL3::SDL3
    OpenGL::GL
)

# On Windows, also link winsock for networking
if(WIN32)
    target_link_libraries(carviary PRIVATE ws2_32 winmm)
endif()

set_target_properties(carviary PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "d:/quake"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "d:/quake"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "d:/quake"
    OUTPUT_NAME "carviary_sdl3"
)
