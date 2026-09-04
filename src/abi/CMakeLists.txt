# cad_abi - the plugin boundary.
#
# Two audiences, one target. `include/cad/abi/cad_abi.h` is pure C and is the
# only file a plugin author needs; everything else is the host side and is C++.
#
# This is also the layer that owns the dynamic loader. cad_core must stay free
# of platform APIs, and LoadLibrary is unambiguously one, so it lives here
# rather than leaking downwards.

add_library(cad_abi STATIC
    src/AbiInternal.cpp
    src/CadApi.cpp
    src/PluginHost.cpp
    src/PluginManager.cpp
)

add_library(cad::abi ALIAS cad_abi)

target_include_directories(cad_abi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(cad_abi
    PUBLIC cad_core
    PRIVATE cad_warnings
)

if(UNIX AND NOT APPLE)
    target_link_libraries(cad_abi PRIVATE ${CMAKE_DL_LIBS})
endif()

set_target_properties(cad_abi PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Abi"
)

# An interface target carrying only the C header.
#
# A plugin links this and nothing else, which is the guarantee made visible in
# the build: if a plugin could accidentally link cad_core, the boundary would be
# decorative. Compiling it as C as well as C++ in the example plugin is what
# keeps the header honestly C-compatible.
add_library(cad_plugin_sdk INTERFACE)
add_library(cad::plugin_sdk ALIAS cad_plugin_sdk)

target_include_directories(cad_plugin_sdk
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
