cmake_minimum_required(VERSION 3.20)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

project(crbridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "crbridge targets Windows only")
endif()

# Iteration 10 dropped the BBuildAndAsyncSendFrame Detours hook in favour
# of an IAT hook on GetModuleHandleA, so the Detours dependency is gone.
# IAT hooking is a direct write to the import table, no library required.

add_library(crbridge SHARED
    src/dllmain.cpp
    src/cr_loader.cpp
    src/cr_iat_hook.cpp
    src/steam_locator.cpp
    src/version_check.cpp
)

target_compile_definitions(crbridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

add_subdirectory(proxy)
