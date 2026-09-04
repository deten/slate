# cad_io - file format readers, writers and resource resolution.
#
# Depends on cad_core and on the standard library only. No Qt, no UI. That keeps
# loading testable headlessly and makes a batch converter or a server-side
# renderer possible without restructuring.
#
# The one platform dependency is process spawning, in OdaFileConverter: DWG
# arrives by running an external translator (ADR 0001), and running a program is
# unavoidably an OS operation. It lives here rather than leaking into cad_core,
# on the same principle that puts the plugin loader in cad_abi.

add_library(cad_io STATIC
    src/DxfTokenizer.cpp
    src/DxfRecord.cpp
    src/DxfEntityReaders.cpp
    src/DxfAnnotationReaders.cpp
    src/DxfReader.cpp
    src/DxfWriter.cpp
    src/ResourceResolver.cpp
    src/XrefLoader.cpp
    src/OdaFileConverter.cpp
    src/RecoveryStore.cpp
)

add_library(cad::io ALIAS cad_io)

target_include_directories(cad_io
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(cad_io
    PUBLIC cad_core
    PRIVATE cad_warnings
)

set_target_properties(cad_io PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Core"
)
