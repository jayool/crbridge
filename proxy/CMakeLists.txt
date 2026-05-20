# Build the version.dll proxy. Outputs "version.dll" (no lib prefix on Windows).
add_library(version_proxy SHARED main.cpp)

set_target_properties(version_proxy PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    SUFFIX ".dll"
)

# x64 con WINAPI (= __stdcall colapsado a Microsoft calling convention en x64):
# nombres exportados son extern "C" sin decorar. No hace falta .def file.
# En x86 sí lo necesitaría — añadirlo si alguna vez se quiere build de 32-bit.

if(MSVC)
    target_compile_options(version_proxy PRIVATE /W3)
endif()

target_link_libraries(version_proxy PRIVATE kernel32)
