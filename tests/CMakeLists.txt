# Test suite.
#
# The tests are ported one-for-one from the TypeScript prototype's suite, which
# is treated as the behavioural specification (see ADR 0002). Where a test's
# expectation encodes a real CAD convention — the DXF bulge sign, the OCS
# 1/64 threshold, snap priority — the comment says so, because those are the
# assertions that must not be "fixed" to match a future regression.

add_library(doctest_main STATIC doctest_main.cpp)
target_include_directories(doctest_main PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
set_target_properties(doctest_main PROPERTIES FOLDER "Tests")

add_executable(cad_tests
    math_tests.cpp
    geometry_tests.cpp
    database_tests.cpp
    command_tests.cpp
    writer_fidelity_tests.cpp
    layout_command_tests.cpp
    attribute_command_tests.cpp
    dimension_chain_tests.cpp
    layer_tool_tests.cpp
    polyline_edit_tests.cpp
    style_command_tests.cpp
    inquiry_command_tests.cpp
    dxf_tests.cpp
    xref_tests.cpp
    render_tests.cpp
    scene_tests.cpp
    picking_tests.cpp
    tracking_tests.cpp
    agent_tests.cpp
    standards_tests.cpp
    drawing_reading_tests.cpp
    project_tests.cpp
    system_command_tests.cpp
    like_command_tests.cpp
    boundary_trace_tests.cpp
    nurbs_tests.cpp
    tessellation_tests.cpp
    roundtrip_tests.cpp
    roundtrip_file_test.cpp
    abi_tests.cpp
    annotation_tests.cpp
    dwg_tests.cpp
    purge_tests.cpp
    property_tests.cpp
    layout_tests.cpp
    field_tests.cpp
    attachment_tests.cpp
    config_tests.cpp
    recovery_tests.cpp
    transfer_tests.cpp
    leader_tests.cpp
    macro_tests.cpp
    mline_tests.cpp
)

target_link_libraries(cad_tests
    PRIVATE
        cad::core
        cad::io
        cad::render
        cad::abi
        doctest_main
        cad_warnings
)

# The plugin-loading tests load the real example plugin rather than a stub.
# A stub would exercise the C++ side of the boundary and none of the part that
# actually breaks: symbol lookup, calling convention, and a separately compiled
# translation unit's view of the structs.
target_compile_definitions(cad_tests PRIVATE
    CAD_ABI_TEST_PLUGIN="$<TARGET_FILE:cad_plugin_boltcircle>"
)
add_dependencies(cad_tests cad_plugin_boltcircle)

set_target_properties(cad_tests PROPERTIES FOLDER "Tests")

# Run from the repository root so the round-trip test can find samples/.
add_test(NAME cad_tests COMMAND cad_tests --reporters=console
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Surface the doctest test cases individually to CTest so a failure names the
# specific case rather than just "the test binary failed".
set_tests_properties(cad_tests PROPERTIES
    FAIL_REGULAR_EXPRESSION "\\[doctest\\] Status: FAILURE"
)
