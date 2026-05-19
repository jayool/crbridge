cmake_minimum_required(VERSION 3.20)

# Force static MSVC runtime for both crbridge AND MinHook subproject.
# Must be set BEFORE project() so FetchContent inherits it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

project(crbridge LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "crbridge targets Windows only")
endif()

# MinHook (static lib) for function hooking
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

add_library(crbridge SHARED
    src/dllmain.cpp
    src/cr_loader.cpp
    src/steam_locator.cpp
    src/vtable_hook.cpp
    src/function_hook.cpp
)

target_compile_definitions(crbridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(crbridge PRIVATE minhook)
