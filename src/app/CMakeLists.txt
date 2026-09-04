# cad_app - the Qt 6 desktop shell.
#
# The ONLY target that links Qt. Everything below it — core, io, render — is
# Qt-free by construction (ADR 0002), which is what keeps the engine usable in
# a headless converter, behind the plugin ABI, and in tests with no display.
#
# The QPainter renderer inside CanvasWidget is an isolated interim backend
# (ADR 0003), not the architecture.

# PrintSupport for plotting. A CAD drawing set that cannot be put on paper is
# not a deliverable, and PDF export goes through the same path.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets PrintSupport Network)

qt_standard_project_setup()

qt_add_executable(cad_app WIN32
    main.cpp
    MainWindow.cpp
    MainWindow.hpp
    CanvasWidget.cpp
    CanvasWidget.hpp
    CommandLine.cpp
    CommandLine.hpp
    Document.cpp
    Document.hpp
    DynamicInput.cpp
    DynamicInput.hpp
    RibbonBar.cpp
    RibbonBar.hpp
    RibbonIcons.cpp
    RibbonLayout.cpp
    RibbonLayout.hpp
    AgentService.cpp
    AgentSettingsDialog.cpp
    HttpAgentProvider.cpp
    ReferenceLibrary.cpp
    ReferenceLibrary.hpp
    LayerOrdering.cpp
    LayerPanel.cpp
    LayerPanel.hpp
    LayerRibbon.cpp
    LayerRibbon.hpp
    ClipboardCommands.cpp
    ViewCommands.cpp
    ViewCommands.hpp
    ImageCache.cpp
    ImageCache.hpp
    InPlaceTextEditor.cpp
    InPlaceTextEditor.hpp
    TextFormatBar.cpp
    TextFormatBar.hpp
    ScenePainter.cpp
    ScenePainter.hpp
    Plotter.cpp
    Plotter.hpp
    QuickSelectDialog.cpp
    QuickSelectDialog.hpp
    PurgeDialog.cpp
    ReferenceManagerDialog.cpp
    ReferenceManagerDialog.hpp
    SettingsDialog.cpp
    SettingsDialog.hpp
    PropertiesPanel.cpp
    PropertiesPanel.hpp
    Theme.cpp
    Theme.hpp
    Branding.hpp
)

# The Windows version resource: what the executable tells the operating system
# it is. Windows Search indexes these strings and an installer reads them, so
# this is the part of the name that does the *finding* — see Slate.rc.
if(WIN32)
    target_sources(cad_app PRIVATE Slate.rc)
endif()

set_target_properties(cad_app PROPERTIES
    # The product is Slate. "Slate CAD" is its full name and lives in the
    # version resource, the installer and the Start menu entry, where a name is
    # searched for rather than read.
    OUTPUT_NAME "Slate"
    FOLDER "App"
)

target_link_libraries(cad_app
    PRIVATE
        cad::core
        cad::io
        cad::render
        cad::abi
        Qt6::Widgets
        Qt6::PrintSupport
        Qt6::Network
        cad_warnings
)

# Qt's lowercase `signals`, `slots` and `emit` macros collide with ordinary
# identifiers in non-Qt code — `Database::emit` was the first casualty. Turning
# them off means a core header can never be made unparseable by the mere
# presence of Qt in a translation unit. The Q_SIGNALS / Q_SLOTS / Q_EMIT
# spellings are used instead.
target_compile_definitions(cad_app PRIVATE QT_NO_KEYWORDS)

# Qt's generated moc code does not satisfy this project's warning level, and
# fixing generated code is not possible. The rule still applies to our sources.
if(MSVC)
    target_compile_options(cad_app PRIVATE /wd4464 /wd5219)
endif()

# The shipped macros, beside the executable where the application looks for
# them. Copied on every build rather than once at install: editing one and
# rebuilding should be enough to try it, and a stale copy of a command the user
# is in the middle of writing is a confusing thing to debug.
add_custom_command(TARGET cad_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/commands"
            "$<TARGET_FILE_DIR:cad_app>/commands"
    COMMENT "Copying the shipped command macros"
    VERBATIM
)

# Copy the Qt runtime next to the executable so the build output is a directory
# that can simply be copied — the stated initial distribution model.
if(WIN32)
    add_custom_command(TARGET cad_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Deploying Qt runtime..."
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-compiler-runtime
                "$<TARGET_FILE:cad_app>"
        COMMENT "Deploying Qt runtime alongside Slate"
        VERBATIM
    )

    # windeployqt ships only the platform plugin the application would use
    # interactively. The offscreen one is how the interface is driven and
    # photographed with no display — see `--run` and `--screenshot` — so it is
    # copied deliberately rather than left to be discovered missing.
    get_target_property(_qt_offscreen_plugin Qt6::QOffscreenIntegrationPlugin LOCATION)
    if(_qt_offscreen_plugin)
        add_custom_command(TARGET cad_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_qt_offscreen_plugin}"
                    "$<TARGET_FILE_DIR:cad_app>/platforms/"
            COMMENT "Adding the offscreen platform plugin for headless runs"
            VERBATIM
        )
    endif()
endif()
