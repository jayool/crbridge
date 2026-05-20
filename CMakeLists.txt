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

# Microsoft Detours — same library LumaCore uses so they chain properly when
# both projects hook the same Steam function. Detours has no top-level CMakeLists,
# so we fetch sources and build a static lib ourselves.
include(FetchContent)
FetchContent_Declare(
    detours
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake-noop
)
FetchContent_MakeAvailable(detours)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(DETOURS_ARCH_SRC "${detours_SOURCE_DIR}/src/disolx64.cpp")
else()
    set(DETOURS_ARCH_SRC "${detours_SOURCE_DIR}/src/disolx86.cpp")
endif()

add_library(detours STATIC
    ${detours_SOURCE_DIR}/src/detours.cpp
    ${detours_SOURCE_DIR}/src/modules.cpp
    ${detours_SOURCE_DIR}/src/disasm.cpp
    ${detours_SOURCE_DIR}/src/image.cpp
    ${detours_SOURCE_DIR}/src/creatwth.cpp
    ${DETOURS_ARCH_SRC}
)
target_include_directories(detours PUBLIC "${detours_SOURCE_DIR}/src")
set_target_properties(detours PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(detours PRIVATE /w)
endif()

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

target_link_libraries(crbridge PRIVATE detours)

add_subdirectory(proxy)
