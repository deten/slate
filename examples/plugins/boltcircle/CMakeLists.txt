# The BOLTCIRCLE example plugin.
#
# A MODULE library, not SHARED: it is loaded at runtime and never linked
# against, and MODULE says so.
#
# It links `cad::plugin_sdk` and nothing else. That is the whole demonstration
# — a plugin gets the C header and no part of the engine, so it cannot come to
# depend on the core's C++ types, and adding a command required no change to
# the engine at all.

add_library(cad_plugin_boltcircle MODULE boltcircle.c)

target_link_libraries(cad_plugin_boltcircle PRIVATE cad::plugin_sdk)

set_target_properties(cad_plugin_boltcircle PROPERTIES
    OUTPUT_NAME "boltcircle"
    # Compiled as C11 on purpose: if the header ever stops being valid C, this
    # target stops building, which is a far better signal than a plugin author
    # discovering it.
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    PREFIX ""
    FOLDER "Examples"
    # Alongside the application, so the default plugins directory finds it and
    # the portable build stays a directory that can be copied.
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)

if(MSVC)
    target_compile_options(cad_plugin_boltcircle PRIVATE /W4 /WX)
else()
    target_compile_options(cad_plugin_boltcircle PRIVATE -Wall -Wextra -Werror)
endif()
