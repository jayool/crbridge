add_library(version_proxy SHARED main.cpp version.def)

set_target_properties(version_proxy PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    SUFFIX ".dll"
)

if(MSVC)
    target_compile_options(version_proxy PRIVATE /W3)
endif()

target_link_libraries(version_proxy PRIVATE kernel32)
