# Small headless utilities.
#
# These exist for verification and batch work, and they are also proof that the
# engine really is UI-free: a converter that links cad_core and cad_io and
# nothing else could not build if the layering had leaked.

add_executable(dxf_roundtrip src/dxf_roundtrip.cpp)

target_link_libraries(dxf_roundtrip
    PRIVATE
        cad::core
        cad::io
        cad_warnings
)

set_target_properties(dxf_roundtrip PROPERTIES FOLDER "Tools")

# Links cad::render as well, so --sheets can build a layout exactly as the
# canvas does. dxf_roundtrip above stays core+io only and is the standing proof
# that the load path itself has no UI dependency.
add_executable(dwg_info src/dwg_info.cpp)

target_link_libraries(dwg_info
    PRIVATE
        cad::core
        cad::io
        cad::render
        cad_warnings
)

set_target_properties(dwg_info PROPERTIES FOLDER "Tools")

# Scene-building benchmark, for the ADR 0003 profiling gate.
add_executable(bench_scene src/bench_scene.cpp)

target_link_libraries(bench_scene
    PRIVATE
        cad::core
        cad::render
        cad_warnings
)

set_target_properties(bench_scene PROPERTIES FOLDER "Tools")