cmake_minimum_required(VERSION 3.20)
project(crbridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "crbridge targets Windows only")
endif()

add_library(crbridge SHARED
    src/dllmain.cpp
    src/cr_loader.cpp
)

# Static MSVC runtime so the DLL has no external CRT dependency at runtime
set_property(TARGET crbridge PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_compile_definitions(crbridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
