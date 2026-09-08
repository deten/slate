# cad_core - the CAD engine.
#
# Hard rule: this target links nothing but the C++ standard library. No Qt, no
# graphics API, no OS UI, no third-party dependency. That constraint is what
# keeps the document model authoritative and independently testable, and it is
# what allows the same core to run in the Qt shell, in a headless batch
# converter, behind the C plugin ABI, or in a test with no display present.
#
# If a dependency is ever added here, it is almost certainly in the wrong layer.

add_library(cad_core STATIC
    src/math/Vec3.cpp
    src/math/Mat4.cpp
    src/math/Box3.cpp

    src/geom/Curve.cpp
    src/geom/LineSegment.cpp
    src/geom/NurbsCurve.cpp
    src/geom/Ocs.cpp
    src/geom/Arc.cpp
    src/geom/EllipseArc.cpp
    src/geom/Polyline.cpp
    src/geom/Intersect.cpp
    src/geom/Operations.cpp
    src/geom/BoundaryTrace.cpp

    src/db/Handle.cpp
    src/db/DbObject.cpp
    src/db/Color.cpp
    src/db/Entity.cpp
    src/db/SymbolTable.cpp
    src/db/Layer.cpp
    src/db/Linetype.cpp
    src/db/TextStyle.cpp
    src/db/DimStyle.cpp
    src/db/BlockTableRecord.cpp
    src/db/Layout.cpp
    src/db/Dictionary.cpp
    src/db/FieldObject.cpp
    src/db/FieldEvaluator.cpp
    src/db/CurveEntities.cpp
    src/db/TextEntities.cpp
    src/db/MTextFormat.cpp
    src/db/DimensionEntity.cpp
    src/db/LeaderEntity.cpp
    src/db/MLeaderEntity.cpp
    src/db/MlineEntity.cpp
    src/db/HatchEntity.cpp
    src/db/ViewportEntity.cpp
    src/db/BlockGeometry.cpp
    src/db/SolidEntity.cpp
    src/db/SplineEntity.cpp
    src/db/InsertEntity.cpp
    src/db/RasterImageEntity.cpp
    src/db/UnderlayEntity.cpp
    src/db/OleFrameEntity.cpp
    src/db/TableEntity.cpp
    src/db/Purge.cpp
    src/db/Database.cpp
    src/db/Transaction.cpp
    src/db/Transfer.cpp
    src/db/Property.cpp
    src/db/SelectionFilter.cpp

    src/snap/ObjectSnap.cpp
    src/snap/Tracking.cpp

    src/agent/AgentPlan.cpp
    src/agent/AgentProvider.cpp
    src/agent/DrawingStandards.cpp
    src/agent/DrawingReading.cpp
    src/agent/AgentDialogue.cpp
    src/command/CommandRegistry.cpp
    src/command/TypedInput.cpp
    src/command/CommandConfig.cpp
    src/command/MacroFile.cpp
    src/config/ConfigFile.cpp
    src/command/CommandExecutor.cpp
    src/command/DrawCommands.cpp
    src/command/PurgeCommand.cpp
    src/command/ModifyCommands.cpp
    src/command/ReshapeCommands.cpp
    src/command/TextCommands.cpp
    src/command/FieldCommands.cpp
    src/command/BlockCommands.cpp
    src/command/LayoutCommands.cpp
    src/command/AttributeCommands.cpp
    src/command/LayerCommands.cpp
    src/command/AnnotationCommands.cpp
)

add_library(cad::core ALIAS cad_core)

target_include_directories(cad_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(cad_core PRIVATE cad_warnings)

set_target_properties(cad_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Core"
)
