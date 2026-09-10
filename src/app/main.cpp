// Slate, a 2D CAD program.
// Copyright (c) 2026 deten
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. It is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License in the LICENSE file for more details.
#include <QApplication>
#include <QCommandLineParser>

#include <optional>
#include <QFont>

#include "Branding.hpp"
#include "MainWindow.hpp"
#include "Theme.hpp"

/// Application entry point.
///
/// Deliberately thin: it configures the application shell and hands off. All
/// behaviour lives below, in layers that know nothing about Qt.
int main(int argc, char* argv[]) {
    QApplication application{argc, argv};
    // The installed product is "Slate CAD"; the application calls itself
    // "Slate". `setApplicationName` is the one the platform reads, so it takes
    // the full name — it reaches the taskbar grouping and the crash reporter,
    // which are places a name is being *looked up* rather than read.
    QApplication::setApplicationName(cad::app::kProductName);
    QApplication::setApplicationDisplayName(cad::app::kDisplayName);
    QApplication::setOrganizationName(cad::app::kDisplayName);
    QApplication::setApplicationVersion("0.2.0");

    // A deliberate typographic baseline rather than the platform default.
    // Segoe UI Variable is the modern Windows UI face; the fallbacks keep the
    // application looking intentional on machines that lack it.
    //
    // Except with no display: the offscreen platform reads fonts through a
    // different engine that renders a variable face as mangled glyphs, which
    // makes a headless screenshot useless for checking labels. A static face
    // costs nothing there — nobody is looking at the typography, they are
    // reading the words.
    const bool headless = QGuiApplication::platformName() == "offscreen";
    QFont font{headless ? "Segoe UI" : "Segoe UI Variable Text", 10};
    font.setStyleHint(QFont::SansSerif);
    QApplication::setFont(font);

    application.setStyleSheet(cad::app::applicationStyleSheet());

    QCommandLineParser parser;
    parser.setApplicationDescription(cad::app::kDescription);
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "Drawing to open (.dxf or .dwg).");

    // Scripted operation. With `-platform offscreen` these drive and photograph
    // the interface without a window appearing, so checking the UI does not
    // take the screen and the keyboard away from whoever is using the machine.
    const QCommandLineOption runOption{
        "run", "Command-line input to run, one entry per --run.", "input"};
    const QCommandLineOption shotOption{
        "screenshot", "Render the window to a PNG and exit.", "path"};
    // Separate from the scripted options because an interactive run can want it
    // too: a capture harness drives a real window and kills it, which leaves
    // recovery files behind and greets the next run with a dialog nobody asked
    // for.
    const QCommandLineOption noRecoveryOption{
        "no-recovery-prompt", "Do not offer to recover work from a previous session."};
    // Pointer options take *canvas* coordinates, and accept "@x,y" for a point
    // in the drawing. Window coordinates would make every one of them depend on
    // the height of the chrome above the canvas, which is how a change to the
    // menus once moved seventeen scenarios' clicks.
    const QCommandLineOption hoverOption{
        "hover", "Put the pointer at x,y on the canvas, or @x,y in the drawing.", "x,y"};
    const QCommandLineOption clickOption{
        "click", "Click the canvas at x,y, or at @x,y in the drawing.", "x,y"};
    const QCommandLineOption shiftClickOption{
        "shift-click", "Shift-click the canvas at x,y, as when marking extra grips.", "x,y"};
    const QCommandLineOption dumpMenusOption{
        "dump-menus", "Print every menu item and whether every command is on one."};
    const QCommandLineOption agentOption{
        "agent", "Ask the configured AI assistant to make a change.", "instruction"};
    const QCommandLineOption agentReplyOption{
        "agent-reply",
        "Answer --agent from this file instead of a provider, for scripted runs.", "file"};
    const QCommandLineOption agentReferenceOption{
        "agent-reference", "Learn house standards from the drawings in this folder.", "folder"};
    const QCommandLineOption agentContextOption{
        "agent-context", "Print what the assistant is told about the drawing, without asking it."};
    const QCommandLineOption projectOption{
        "project", "Use this project file for the session, whatever sits beside the drawing.",
        "file"};
    const QCommandLineOption makeSheetsOption{
        "make-sheets", "Make the file of every listed sheet that has none, from the template."};
    const QCommandLineOption attachBackgroundOption{
        "attach-background",
        "Overlay a background from the project on the open drawing; empty for the sheet's own.",
        "name"};
    const QCommandLineOption checkProjectOption{
        "check-project", "Check the set and print what is wrong with it."};
    const QCommandLineOption panelOption{
        "panel", "Show a dock panel: layers, properties, history, references.", "name"};
    const QCommandLineOption layoutOption{
        "layout", "Switch to a paper space layout by name.", "name"};
    const QCommandLineOption menuPickOption{
        "menu-pick", "Trigger an item of the menu last opened, by its label.", "label"};
    const QCommandLineOption snapMenuOption{
        "snap-menu", "Shift with the right button here, for the snap override menu.", "x,y"};
    const QCommandLineOption rightHoldOption{
        "right-hold", "Hold the right button here until the context menu opens.", "x,y"};
    const QCommandLineOption doubleClickOption{
        "double-click", "Double-click the canvas at x,y.", "x,y"};
    const QCommandLineOption typeOption{
        "type", "Send text as keystrokes to whatever holds the keyboard.", "text"};
    const QCommandLineOption keyOption{
        "key", "Send one key: return, escape, ctrl+return.", "key"};
    const QCommandLineOption openOption{
        "open", "Open a drawing part-way through a script.", "path"};
    const QCommandLineOption dropOption{
        "drop", "Drop a file onto the window, as when dragged from a folder.", "path"};
    const QCommandLineOption dialogShotOption{
        "dialog-shot", "Photograph a dialog, as name=path.", "name=path"};
    const QCommandLineOption sizeOption{
        "size", "Resize the window before anything else, as WIDTHxHEIGHT.", "WxH"};
    const QCommandLineOption canvasOption{
        "canvas", "Size the window so the drawing area is exactly WIDTHxHEIGHT.", "WxH"};
    const QCommandLineOption saveAsOption{
        "save-as", "Save the drawing to this path and exit. The extension picks the format.",
        "path"};
    const QCommandLineOption tabOption{
        "tab", "Bring the named ribbon tab to the front.", "title"};
    const QCommandLineOption cycleOption{
        "cycle", "Step through overlapping objects this many times.", "count"};
    const QCommandLineOption pdfOption{
        "export-pdf", "Plot the current layout to a PDF and exit.", "path"};
    const QCommandLineOption plotImageOption{
        "plot-image", "Render the plot to a PNG, for inspecting output.", "path"};
    parser.addOption(runOption);
    parser.addOption(shotOption);
    parser.addOption(noRecoveryOption);
    parser.addOption(hoverOption);
    parser.addOption(clickOption);
    parser.addOption(shiftClickOption);
    parser.addOption(dumpMenusOption);
    parser.addOption(agentOption);
    parser.addOption(agentReplyOption);
    parser.addOption(agentReferenceOption);
    parser.addOption(agentContextOption);
    parser.addOption(projectOption);
    parser.addOption(makeSheetsOption);
    parser.addOption(attachBackgroundOption);
    parser.addOption(checkProjectOption);
    parser.addOption(panelOption);
    parser.addOption(layoutOption);
    parser.addOption(menuPickOption);
    parser.addOption(snapMenuOption);
    parser.addOption(rightHoldOption);
    parser.addOption(doubleClickOption);
    parser.addOption(typeOption);
    parser.addOption(keyOption);
    parser.addOption(openOption);
    parser.addOption(dropOption);
    parser.addOption(dialogShotOption);
    parser.addOption(sizeOption);
    parser.addOption(canvasOption);
    parser.addOption(saveAsOption);
    parser.addOption(tabOption);
    parser.addOption(cycleOption);
    parser.addOption(pdfOption);
    parser.addOption(plotImageOption);
    parser.process(application);

    cad::app::MainWindow window;
    const bool scripted = parser.isSet(runOption) || parser.isSet(shotOption) ||
                          parser.isSet(hoverOption) || parser.isSet(cycleOption) ||
                          parser.isSet(clickOption) || parser.isSet(doubleClickOption) || parser.isSet(rightHoldOption) || parser.isSet(snapMenuOption) ||
                          parser.isSet(typeOption) || parser.isSet(keyOption) ||
                          parser.isSet(pdfOption) || parser.isSet(plotImageOption) ||
                          parser.isSet(saveAsOption) || parser.isSet(tabOption) ||
                          parser.isSet(dialogShotOption) || parser.isSet(openOption) ||
                          parser.isSet(shiftClickOption) || parser.isSet(menuPickOption) ||
                          parser.isSet(layoutOption) || parser.isSet(panelOption) ||
                          parser.isSet(agentOption) || parser.isSet(agentReplyOption) ||
                          parser.isSet(agentReferenceOption) ||
                          parser.isSet(agentContextOption) || parser.isSet(projectOption) ||
                          parser.isSet(makeSheetsOption) ||
                          parser.isSet(attachBackgroundOption) ||
                          parser.isSet(checkProjectOption) ||
                          parser.isSet(dumpMenusOption) ||
                          parser.isSet(sizeOption) || parser.isSet(canvasOption);
    if (scripted || parser.isSet(noRecoveryOption)) {
        // Nobody is watching to answer a modal dialog.
        window.setNonInteractive();
    }
    // Before showing, so the first layout is the one being asked about. A
    // ribbon that fits at one width and not another is exactly the kind of
    // thing worth photographing at both.
    if (parser.isSet(sizeOption)) {
        const QStringList parts = parser.value(sizeOption).split('x', Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            window.resize(parts[0].toInt(), parts[1].toInt());
        }
    }
    window.show();

    // After show: the canvas has no size until the window has been laid out,
    // and this works by measuring it and correcting.
    if (parser.isSet(canvasOption)) {
        const QStringList parts = parser.value(canvasOption).split('x', Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            window.resizeCanvasTo(parts[0].toInt(), parts[1].toInt());
        }
    }

    // Opening from the command line is how a drawing arrives when the user
    // double-clicks a file, so it is supported from the first version.
    // Every file named, not only the first: several drawings on one command
    // line is how a set arrives from a shell or a drag onto the application,
    // and each one opens in its own tab.
    for (const QString& file : parser.positionalArguments()) {
        window.openFile(file);
    }

    if (scripted) {
        // Replayed in the order they were *typed*, across options.
        //
        // `QCommandLineParser` groups values by option, which meant every
        // `--run` happened before every `--click` however they were written.
        // That is fine for "draw this, then photograph it" and useless for
        // anything interactive: editing a label is click the label, type the
        // new words, and there was no way to say that. Walking the raw
        // arguments is what restores the order the author intended.
        bool photographed = false;
        const QStringList arguments = QCoreApplication::arguments();
        const auto pointAt = [&window](const QString& spec) -> std::optional<QPoint> {
            // "@x,y" is a point in the drawing, resolved through the current
            // view. It says what a scenario means -- "the corner at 400,260" --
            // rather than the pixel that corner happens to occupy today.
            const bool inDrawing = spec.startsWith(QLatin1Char{'@'});
            const QStringList parts = (inDrawing ? spec.mid(1) : spec).split(',');
            if (parts.size() != 2) {
                return std::nullopt;
            }
            if (inDrawing) {
                return window.worldToCanvas(parts[0].trimmed().toDouble(),
                                            parts[1].trimmed().toDouble());
            }
            return QPoint{parts[0].trimmed().toInt(), parts[1].trimmed().toInt()};
        };

        for (int i = 1; i < arguments.size(); ++i) {
            const QString& argument = arguments.at(i);
            if (argument == "--dump-menus") {
                window.dumpMenus();
                continue;
            }
            if (argument == "--agent-context") {
                window.showAgentContext();
                continue;
            }
            if (argument == "--make-sheets") {
                window.makeSheets();
                continue;
            }
            if (argument == "--check-project") {
                window.checkProject();
                continue;
            }
            const bool takesValue = argument == "--run" || argument == "--hover" ||
                                    argument == "--click" || argument == "--shift-click" ||
                                    argument == "--menu-pick" ||
                                    argument == "--layout" ||
                                    argument == "--panel" ||
                                    argument == "--agent" || argument == "--agent-reply" ||
                                    argument == "--agent-reference" || argument == "--project" ||
                                    argument == "--attach-background" ||
                                    argument == "--double-click" || argument == "--right-hold" || argument == "--snap-menu" ||
                                    argument == "--type" || argument == "--key" ||
                                    argument == "--cycle" || argument == "--tab" ||
                                    argument == "--save-as" || argument == "--dialog-shot" ||
                                    argument == "--open" || argument == "--drop" ||
                                    argument == "--screenshot";
            if (!takesValue || i + 1 >= arguments.size()) {
                continue;
            }
            const QString value = arguments.at(++i);

            if (argument == "--run") {
                window.runScript({value});
            } else if (argument == "--hover") {
                // Some behaviour depends on where the pointer has *been*, not
                // only where it is: extension snapping is acquired by hovering
                // an object and then moving away from it.
                if (const auto point = pointAt(value)) {
                    window.hoverAt(*point);
                }
            } else if (argument == "--click") {
                if (const auto point = pointAt(value)) {
                    window.clickAt(*point);
                }
            } else if (argument == "--shift-click") {
                if (const auto point = pointAt(value)) {
                    window.clickAt(*point, Qt::ShiftModifier);
                }
            } else if (argument == "--dump-menus") {
                window.dumpMenus();
            } else if (argument == "--agent-reply") {
                // Set before any --agent runs, so order on the command line
                // matters and reads the way it is written.
                window.useCannedAgentReply(value);
            } else if (argument == "--agent-reference") {
                window.useAgentReference(value);
            } else if (argument == "--project") {
                window.useProject(value);
            } else if (argument == "--attach-background") {
                window.attachBackground(value);
            } else if (argument == "--agent") {
                window.runAgent(value);
            } else if (argument == "--panel") {
                static_cast<void>(window.showPanel(value));
            } else if (argument == "--layout") {
                static_cast<void>(window.showLayout(value));
            } else if (argument == "--menu-pick") {
                static_cast<void>(window.pickMenuAction(value));
            } else if (argument == "--snap-menu") {
                if (const auto point = pointAt(value)) {
                    window.snapMenuAt(*point);
                }
            } else if (argument == "--right-hold") {
                if (const auto point = pointAt(value)) {
                    window.rightHoldAt(*point);
                }
            } else if (argument == "--double-click") {
                if (const auto point = pointAt(value)) {
                    window.doubleClickAt(*point);
                }
            } else if (argument == "--type") {
                window.typeText(value);
            } else if (argument == "--key") {
                window.pressKey(value);
            } else if (argument == "--drop") {
                static_cast<void>(window.dropFilesForScript({value}));
            } else if (argument == "--open") {
                // Opening *during* a script rather than only before it, so a
                // scene can show what a second drawing does to the window.
                window.openFile(value);
            } else if (argument == "--screenshot") {
                // Replayed in place rather than only at the end, so a script can
                // photograph before *and* after the thing it is testing. Taking
                // just the last one made "does Escape clear this?" unanswerable
                // without two runs.
                if (!window.grabTo(value)) {
                    return 1;
                }
                photographed = true;
            } else if (argument == "--dialog-shot") {
                const qsizetype split = value.indexOf('=');
                if (split <= 0 || !window.grabDialogTo(value.left(split),
                                                       value.mid(split + 1))) {
                    qWarning("could not photograph dialog \"%s\"", qUtf8Printable(value));
                    return 1;
                }
            } else if (argument == "--save-as") {
                if (!window.saveDrawingTo(value)) {
                    qWarning("could not save to \"%s\"", qUtf8Printable(value));
                    return 1;
                }
            } else if (argument == "--tab") {
                if (!window.showRibbonTab(value)) {
                    qWarning("no ribbon tab named \"%s\"", qUtf8Printable(value));
                    return 1;
                }
            } else if (argument == "--cycle") {
                for (int step = 0; step < value.toInt(); ++step) {
                    window.cycleSelection();
                }
            }
        }
        if (parser.isSet(pdfOption) && !window.exportPdfTo(parser.value(pdfOption))) {
            return 1;
        }
        if (parser.isSet(plotImageOption) && !window.plotToImage(parser.value(plotImageOption))) {
            return 1;
        }
        // Only if the replay did not already take it, which it does whenever
        // `--screenshot` appears among the scripted arguments.
        if (parser.isSet(shotOption) && !photographed) {
            if (!window.grabTo(parser.value(shotOption))) {
                return 1;
            }
        }
        return 0;
    }

    return QApplication::exec();
}
