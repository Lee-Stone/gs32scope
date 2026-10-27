cmake_minimum_required(VERSION 3.21)

project(GS32Scope
    VERSION 0.1.0
    DESCRIPTION "GS32Scope - online variable oscilloscope"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

set(GS32_DRIVERS_DIR "${CMAKE_BINARY_DIR}/drivers" CACHE INTERNAL "")
file(MAKE_DIRECTORY ${GS32_DRIVERS_DIR})

add_subdirectory(gs32link)
add_subdirectory(app)
add_subdirectory(tests)
