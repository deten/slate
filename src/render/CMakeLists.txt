# cad_render - the rendering abstraction.
#
# Backend-agnostic by design (ADR 0003): scene building, viewport transforms,
# spatial culling and level of detail all produce plain data, so they are
# testable headlessly with no GPU present. A graphics backend consumes that
# data; it does not appear in this target's interface.
#
# cad_core must never depend on this, only the other way round.

add_library(cad_render STATIC
    src/Viewport.cpp
    src/SpatialIndex.cpp
    src/Scene.cpp
    src/Picking.cpp
)

add_library(cad::render ALIAS cad_render)

target_include_directories(cad_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(cad_render
    PUBLIC cad_core
    PRIVATE cad_warnings
)

set_target_properties(cad_render PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Render"
)
