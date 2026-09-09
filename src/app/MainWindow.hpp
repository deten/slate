#pragma once

#include <filesystem>

#include <QMainWindow>
#include <QPointer>
#include <map>
#include <memory>

#include "CanvasWidget.hpp"
#include "Document.hpp"
#include "cad/render/Viewport.hpp"
#include "LayerPanel.hpp"
#include "Plotter.hpp"
#include "LayerRibbon.hpp"
#include "PropertiesPanel.hpp"
#include "RibbonBar.hpp"
#include "ViewCommands.hpp"
#include "cad/io/OdaFileConverter.hpp"
#include "cad/io/RecoveryStore.hpp"

class QDockWidget;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QShortcut;
class QTabBar;
class QPrinter;
class QTimer;
class QToolButton;

namespace cad::app {

class AgentService;
class ProjectIndex;
class ProjectPanel;

class CanvasWidget;
class CommandLine;

/// The application window.
///
/// Canvas-centric: the drawing occupies the whole window and supporting panels
/// dock around it. The window owns the *document* and hands the canvas and
/// command line a pointer to it, so opening a file swaps one object and nothing
/// is left pointing at a destroyed database.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    /// Open a drawing, resolving its external references.
    void openFile(const QString& path);
    /// Drop files onto the window as the desktop would, for a scripted run.
    ///
    /// Goes through the real drag-enter and drop events rather than calling
    /// openFile directly, so a script exercises the same acceptance and
    /// routing a person's drag does.
    /// @returns false when nothing dropped was a drawing.
    bool dropFilesForScript(const QStringList& paths);

    /// The local .dwg/.dxf paths in a drag, if any.
    [[nodiscard]] static QStringList droppedDrawings(const QMimeData* mime);

    /// Run lines of command input as though typed, for scripted testing.
    ///
    /// Together with `--platform offscreen` and `grabTo`, this is how the
    /// interface is checked without a window appearing and stealing the
    /// keyboard from whatever the person at the machine is doing.
    void runScript(const QStringList& lines);

    /// Render the window to an image file. Works with no display.
    bool grabTo(const QString& path);

    /// Step to the next overlapping object, as Shift+Space does.
    void cycleSelection();

    /// Hold the right button long enough to raise the context menu.
    void rightHoldAt(const QPoint& canvasPosition);

    /// Shift with the right button, raising the snap override menu.
    void snapMenuAt(const QPoint& canvasPosition);

    /// Bring a ribbon tab to the front by title. False when there is no such
    /// tab — which is the answer a visual check is asking for.
    bool showRibbonTab(const QString& title);

    /// Show a dock panel by name, for scripted runs. False if there is none.
    ///
    /// Some panels start hidden -- the layer manager sits behind the ribbon's
    /// own controls -- which meant a scripted run could not photograph them and
    /// the sweep had never looked at one.
    bool showPanel(const QString& name);

    /// Every command name reachable from the menu bar.
    [[nodiscard]] QStringList menuCoverage() const;

    /// Print the whole menu tree and whether every command is on it, for
    /// scripted runs. The coverage check lives here rather than in the script
    /// so that both answer from the same walk.
    void dumpMenus();

    /// Hand an instruction to the configured assistant, show what it proposes,
    /// and apply it once approved. Public because a scripted run drives it
    /// directly, which is the same path the AI command takes.
    void runAgent(const QString& instruction);

    /// Answer from a file instead of a provider, for scripted runs.
    void useCannedAgentReply(const QString& path);
    /// Learn house standards from this folder for this run, whatever the
    /// settings say.
    void useAgentReference(const QString& directory);
    /// Use this project file for the session, whatever sits beside the
    /// drawing. For scripted runs and for a set kept away from its files.
    void useProject(const QString& path);
    /// Make the file of every listed sheet that has none, from the project's
    /// template. What the Project tool's button does, for scripted runs.
    void makeSheets();
    /// Attach a background from the project to the open drawing the office
    /// way: as an overlay, placed as the sibling sheets place it, with the
    /// project's frozen layers frozen. An empty name means the background
    /// the open sheet is listed with.
    void attachBackground(const QString& name);
    /// Print what the assistant would be told, without asking it.
    void showAgentContext();
    /// The assistant, made on first use.
    AgentService& ensureAgent();

    /// Switch to a layout by name, for scripted runs. False if there is none.
    bool showLayout(const QString& name);

    /// Save to a path chosen by a script rather than by a file dialog. The
    /// extension picks the format, exactly as it does for Save As.
    bool saveDrawingTo(const QString& path);

    /// Open a dialog by name, photograph it, and close it.
    ///
    /// Dialogs are the part of the interface a screenshot of the main window
    /// never shows, and they are where a list the user has to read lives. A
    /// modal `exec` would stop a scripted run dead, so this shows and grabs
    /// instead.
    bool grabDialogTo(const QString& name, const QString& path);

    /// Render a plot to an image, at the given pixel width.
    ///
    /// The plot path produces paper, which is exactly the thing that cannot be
    /// inspected in a test. This runs the same `paintPlot` into something that
    /// can be looked at and compared pixel for pixel.
    bool plotToImage(const QString& path, int widthPixels = 1600);

    /// Plot the current layout straight to a PDF, with no dialog.
    ///
    /// The same path Export PDF uses, so a scripted plot and a plotted one
    /// cannot diverge. Also makes batch plotting a drawing set possible without
    /// the UI at all.
    bool exportPdfTo(const QString& path);

    /// Click at a point on the canvas, as a user would.
    ///
    /// Canvas coordinates, not window ones: what a scripted click is aimed at
    /// is geometry, and where geometry lands depends on the canvas and nothing
    /// else. Addressing the window instead made every scenario's coordinates
    /// depend on the height of the chrome above the drawing. Negative values
    /// are legal and reach the panels and the ribbon.
    void clickAt(const QPoint& canvasPosition);
    void clickAt(const QPoint& canvasPosition, Qt::KeyboardModifiers modifiers);

    /// Where a drawing coordinate currently is on the canvas.
    ///
    /// Lets a scenario say which *point of the drawing* it is aiming at rather
    /// than working out the pixel by hand.
    [[nodiscard]] QPoint worldToCanvas(double x, double y) const;

    /// Resize the window until the canvas is exactly this size. False, with a
    /// line in the history, if it could not be made to settle there.
    bool resizeCanvasTo(int width, int height);

    /// Trigger an item of the menu a scripted run last opened, by its label.
    ///
    /// Scripted runs cannot click a popup: it is its own top-level window, and
    /// a synthetic click at its coordinates lands on the canvas behind it.
    /// Without this the harness could read a menu's labels but never find out
    /// whether any of them did anything. Returns false if no menu is open or
    /// nothing matches, so a script naming a stale label fails loudly.
    bool pickMenuAction(const QString& label);

    /// Trigger a menu bar item by its label or by the command it runs.
    bool pickMenuBarAction(const QString& label);
    void doubleClickAt(const QPoint& canvasPosition);

    /// The deepest widget under a window position, so a scripted click can
    /// reach the ribbon and the panels rather than only the drawing.
    [[nodiscard]] QWidget* widgetAt(const QPoint& windowPosition);

    /// Send `text` as key events to whatever holds the keyboard. The only way
    /// to reach a widget that is not the command line — the in-place text
    /// editor, most of all.
    void typeText(const QString& text);

    /// Send one named key, optionally with modifiers: `return`, `escape`,
    /// `ctrl+return`.
    void pressKey(const QString& spec);

    /// Move the pointer over the canvas, in canvas coordinates.
    ///
    /// Sends the same event a real pointer would, so rollover highlighting and
    /// snap markers can be checked without a window, a mouse, or taking the
    /// screen away from whoever is using the machine.
    void hoverAt(const QPoint& canvasPosition);

    /// A canvas coordinate in the window's own frame, for the few things that
    /// need to know which widget is under a point.
    [[nodiscard]] QPoint canvasToWindow(const QPoint& canvasPosition);

    /// Run without dialogs.
    ///
    /// A scripted session has nobody to answer a modal dialog, and one raised
    /// under `--run` blocks the process for ever. Anything that would open one
    /// reports to the history instead — which also makes those paths reachable
    /// from a test.
    void setNonInteractive() noexcept { nonInteractive_ = true; }

protected:
    /// Offers to save unsaved work before the window closes.
    void closeEvent(QCloseEvent* event) override;

    /// A drawing dragged from a folder and dropped anywhere on the window
    /// opens in a new tab, as File > Open would.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    /// Escape, when nothing nearer to the keyboard has already taken it.
    void keyPressEvent(QKeyEvent* event) override;

    /// Watches the whole application for Escape.
    ///
    /// The command line is meant to hold the keyboard, and mostly does -- but
    /// clicking a panel, a ribbon button or a layer list moves focus, and those
    /// widgets swallow Escape rather than letting it reach the window. The user
    /// presses it, nothing happens, and the command they are trying to get out
    /// of is still running. Watching from above is the only place that sees the
    /// key whatever has focus.
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    void onOpen();
    void onNew();
    void onSave();
    void onSaveAs();
    void onZoomExtents();
    void onCursorMoved(double worldX, double worldY);
    void onViewChanged();
    /// Cancel a running command, or failing that clear the selection.
    /// @returns true when there was something to stop.
    bool escapePressed();

    void onEditBlock(Handle blockRecord);

    /// The menu the right button raises when held.
    ///
    /// Built from what is actually happening, as other CAD software's is: a running
    /// command offers Enter and Cancel, a selection offers what can be done to
    /// it, and an empty drawing area offers the last command again. A menu that
    /// listed everything regardless would be a menu nobody reads.
    void showCanvasContextMenu(const QPoint& globalPosition);

    /// The snap override menu, on Shift with the right button.
    void showSnapOverrideMenu(const QPoint& globalPosition);

    /// Show a menu, modally for a person and non-modally for a script.
    void popupMenu(QMenu& menu, const QPoint& globalPosition);
    /// Turn ortho on or off and say so, from the button or from F8.
    void toggleOrtho();

    /// Turn polar tracking on or off and say so, from the button or from F10.
    void togglePolar();

    /// Turn the running object snaps on or off, from the button or from F3.
    void toggleObjectSnap();
    void toggleTracking();
    /// Show lineweights at their width on screen, or as hairlines.
    void toggleLineweights();
    void onAgentSettings();
    void onProject();

    void onPurge();
    void onReferenceManager();
    /// Make the block record for a drawing attached as an external reference
    /// and load its contents. Returns the problem, or empty and the handle.
    [[nodiscard]] std::string attachExternalReference(const std::string& path, bool overlay,
                                                      Handle* block);
    /// What the attach commands need from the file layer.
    [[nodiscard]] AttachHooks attachHooks();
    void onSelectionChanged();
    void onCommandStateChanged();
    void onUndo();
    void onRedo();
    void onLayoutTabChanged(int index);
    void onReloadConfig();
    void onOpenConfigFile();
    void onDiscardRecoveryFiles();
    void onSettings();
    void onQuickSelect();
    void onPrint();
    void onPrintPreview();
    void onExportPdf();
    void onPlotSetup();

private:
    void buildUi();

    /// The menu bar, built once, and hidden unless the user asks for it.
    ///
    /// It still holds every menu — the fixed application actions here, the
    /// generated command entries from `buildCommandMenus` — because it is
    /// where they are *defined*. It is simply not where they are normally
    /// *shown*: the ribbon's Menu button puts the same menus on one row with
    /// the tabs, and View > Menu Bar brings the classic row back.
    void buildMenus();

    /// Keep the fixed menu accelerators live while the menu bar is hidden.
    void registerMenuShortcuts();

    /// Point the ribbon's Menu button at whatever is currently on the menu bar.
    void syncAppMenu();

    /// The ribbon's quick-access strip and its wiring, built once.
    void buildRibbon();

    /// Repopulate the ribbon's tabs from the document's command registry.
    void refreshRibbon();

    /// Mirror the command line's prompt and text under the cursor.
    void refreshDynamicInput();

    /// Put the keyboard back in the command line, where typing always goes.
    void focusCommandLine();

    /// Move the active layout tab by `delta`, wrapping.
    void stepLayout(int delta);

    /// The per-user folder holding configuration and recovery copies.
    [[nodiscard]] static QString settingsDirectory();

    /// Where the user's configuration lives.
    [[nodiscard]] static QString configPath();

    /// Bring across the settings and unsaved work of a previous name.
    ///
    /// Runs once, when this application's folder does not exist and the older
    /// one does. Renaming a product renames the folder it keeps a user's
    /// recovery files in, and a user who had unsaved work waiting would have
    /// found a fresh application looking somewhere that had never existed.
    void adoptPreviousSettings();

    /// Recursive copy, for the above. Returns false on the first failure.
    [[nodiscard]] static bool copyDirectory(const QString& from, const QString& to);

    /// Read it, writing a documented starter file on first run.
    void loadConfig();

    /// Note in the history what the configuration did, successes and failures
    /// alike.
    void reportConfig();

    /// Rebuild the keyboard shortcuts from the configuration.
    void applyShortcuts();

    /// Build the status-bar object-snap control and load its saved state.
    ///
    /// In the status bar rather than a dialog because it is toggled constantly
    /// while drafting — reaching a modal dialog to turn off Perpendicular for
    /// one pick is the kind of friction that makes people give up on snaps and
    /// zoom in instead.
    void buildSnapControl();

    /// Apply a snap mask to the canvas, refresh the menu, and remember it.
    void setSnapModes(std::uint32_t modes);

    /// Push every configurable setting into the widgets that honour it.
    ///
    /// One path, so the settings dialog, a hand-edited file and a reload all
    /// produce the same result.
    void applySettings();

    /// Start or restart the auto-save timer from the configuration.
    void configureAutoSave();

    /// Write a recovery copy, if there is unsaved work and it is safe to do so.
    void autoSave();

    /// Offer back anything an interrupted session left behind. Called once, at
    /// startup, before the user has done any work worth losing.
    void offerRecovery();

    /// Rename an existing file out of the way before overwriting it.
    void writeBackup(const QString& path) const;

    /// What the current tab would plot: the active sheet, or model space.
    [[nodiscard]] PlotRequest currentPlotRequest() const;

    /// Render one page onto a printer or PDF writer.
    void renderPlot(QPrinter& printer);

    /// Report a failure: always to the history, and in a dialog only when
    /// somebody is there to dismiss it.
    void reportProblem(const QString& title, const QString& message,
                       const QString& remedy = {}, const QString& detail = {});

    /// Say what the plot left blank, so a hole in a sheet is never a surprise
    /// discovered on paper.
    void reportOmittedContent(std::size_t count);

    /// Remember a file in the recently-opened list.
    void rememberRecentFile(const QString& path);
    void rebuildRecentFilesMenu();

    /// Point every panel at the current document, so none is left reading a
    /// database that has been replaced.
    void setPanelDocument();

    void runCommand(const char* name);


    /// Where macro files are read from: the installed folder, then the user's.
    [[nodiscard]] static std::vector<std::filesystem::path> macroFolders();

    /// Write anything that would not load into the command history.
    void reportMacroProblems();

    /// Build one menu per command category, from the registry.
    ///
    /// Called whenever the set of commands can have changed -- a new document
    /// brings its own registry, and a plugin's commands arrive with it.
    void buildCommandMenus();

    /// Create a document wired to this installation's plugins directory.
    ///
    /// Not const: the commands it registers call back into the window to
    /// switch layouts, which refreshes the tab bar and the view.
    [[nodiscard]] std::unique_ptr<Document> makeDocument();

    /// Report what the document's plugin pass found, successes and failures
    /// alike. A plugin that failed to load must say so — silence looks
    /// identical to a plugin that loaded and did nothing.
    void reportPlugins();

    /// Note in the command history whether DWG translation is possible, before
    /// attempting it.
    void reportTranslatorIfUnavailable();

    /// Every location the DWG translator was looked for, for the failure
    /// dialog's details pane.
    [[nodiscard]] QString translatorSearchReport() const;

    /// Whether this build can produce a DWG right now: a translator exists,
    /// writes, and is actually installed. Asked before offering DWG anywhere,
    /// so the offer is never one that fails when taken.
    [[nodiscard]] bool canSaveDwg() const;

    /// Write the document to `path`. Returns false and reports on failure.
    bool saveTo(const QString& path);

    /// Prompt to save when there are unsaved changes.
    /// @returns false when the user cancels, meaning the caller must not
    ///   discard the document.
    [[nodiscard]] bool confirmDiscardChanges();
    void refreshLayerPanel();

    /// Rebuild the layout tab bar from the document's layouts.
    void refreshLayoutTabs();

    /// Rebuild the file tab bar from the open drawings.
    void refreshFileTabs();

    /// Make `index` the drawing in front of the user, saving the outgoing one's
    /// view and history first.
    void activateDrawing(int index);

    /// Close a drawing, asking about unsaved changes first.
    /// @returns false when the user cancelled.
    bool closeDrawing(int index);

    /// Adopt a freshly built document as a new tab and show it.
    void addDrawing(std::unique_ptr<Document> document, const QString& path);

    /// The index of `document_` in `drawings_`, or -1.
    [[nodiscard]] int activeDrawingIndex() const;
    void refreshCommandLog();
    void updateWindowTitle();
    void log(const QString& message);

    /// The user's configuration: aliases, shortcuts and settings.
    ///
    /// Owned by the window rather than the document, like the DWG translator:
    /// it is a property of the installation, and every drawing opened in this
    /// session shares it.
    ///
    /// Declared *before* `document_` deliberately. Documents hold a raw pointer
    /// to it, and members are destroyed in reverse declaration order, so this
    /// must be declared first to be destroyed last.
    ConfigFile config_;

    /// One open drawing and the shell state that belongs to it rather than to
    /// the window.
    ///
    /// The view and the command history are per drawing because they describe
    /// *that* drawing: switching tabs and finding a different zoom, or somebody
    /// else's command history, would make the tabs feel like one drawing being
    /// reloaded rather than several being held open.
    struct OpenDrawing {
        std::unique_ptr<Document> document;
        QString path;
        render::Viewport view;
        bool hasView = false;
        QString log;
        std::size_t loggedEntries = 0;
    };

    /// Every open drawing, in tab order.
    ///
    /// Declared after `config_` for the same reason `document_` was: each one
    /// holds a raw pointer to the configuration and must be destroyed first.
    std::vector<OpenDrawing> drawings_;

    /// The drawing being edited, pointing into `drawings_`.
    ///
    /// A raw pointer rather than an index because a hundred call sites already
    /// say `document_->`, and what they mean -- the drawing in front of the
    /// user -- has not changed.
    Document* document_ = nullptr;
    QString currentPath_;

    /// Bindings created from the configuration, owned so a reload can replace
    /// them rather than accumulate a second set.
    std::vector<QShortcut*> shortcuts_;

    /// Auto-save. Declared after `config_` because it reads its interval from
    /// there, and holds no reference to the document — it asks for one each
    /// time it fires, so a document swapped underneath it is not a problem.
    std::unique_ptr<io::RecoveryStore> recovery_;
    QTimer* autoSaveTimer_ = nullptr;

    QMenu* recentFilesMenu_ = nullptr;
    /// The ribbon Menu button's popup: the menu bar's own menus, shown in a
    /// second place rather than copied into one.
    QStringList macroProblems_;
    QMenu* appMenu_ = nullptr;
    /// View > Menu Bar, checkable, mirroring the bar's visibility.
    QAction* menuBarAction_ = nullptr;
    /// The popup a scripted run last opened, for `pickMenuAction`. A guarded
    /// pointer because the menu deletes itself on close.
    QPointer<QMenu> scriptedMenu_;
    bool nonInteractive_ = false;
    /// Set when settings were brought across from the previous name, so the
    /// history can say so once the window exists to say it in.
    bool pendingMigrationNotice_ = false;

    /// The status-bar snap control and one checkable action per mode.
    QToolButton* snapButton_ = nullptr;
    /// ORTHO, beside the snap control, as other CAD software has it.
    QToolButton* orthoButton_ = nullptr;
    /// POLAR, the other constraint, on F10.
    QToolButton* polarButton_ = nullptr;
    QToolButton* trackingButton_ = nullptr;
    QToolButton* lineweightButton_ = nullptr;
    std::unique_ptr<AgentService> agent_;
    /// The set the open drawing belongs to, found beside it or opened by
    /// hand; empty when there is none. Never required.
    std::unique_ptr<ProjectIndex> project_;
    [[nodiscard]] ProjectIndex& ensureProject();
    void openProjectBeside(const QString& drawingPath);
    /// Show the panel what the project and the open drawing now are, and
    /// bring it up the first time a project appears.
    void refreshProjectPanel();
    /// Menus made for a command category, torn down when they are rebuilt.
    std::vector<QMenu*> commandMenus_;
    /// Actions appended to a menu that already existed, kept so a rebuild can
    /// remove exactly what it added and leave the curated items alone.
    std::map<QMenu*, std::vector<QAction*>> appendedCommandActions_;
    std::vector<std::pair<SnapMode, QAction*>> snapActions_;
    /// The mask to restore when snapping is switched back on, so turning it off
    /// and on again does not silently reset the user's choices.
    std::uint32_t savedSnapModes_ = kDefaultSnapModes;

    /// How DWG data enters the application (ADR 0001).
    ///
    /// Owned by the window rather than by the document: it is a property of
    /// the installation, not of a drawing, and it is needed *before* a document
    /// exists in order to open one.
    std::unique_ptr<io::DwgTranslator> translator_;

    RibbonBar* ribbon_ = nullptr;
    CanvasWidget* canvas_ = nullptr;
    CommandLine* commandLine_ = nullptr;
    LayerPanel* layers_ = nullptr;
    LayerRibbon* layerRibbon_ = nullptr;
    PropertiesPanel* properties_ = nullptr;
    QTabBar* layoutTabs_ = nullptr;
    /// One tab per open drawing, above the drawing area as other CAD software puts them.
    QTabBar* fileTabs_ = nullptr;
    /// Set while the file tab bar is being rebuilt, so its own signals are not
    /// mistaken for the user choosing a tab.
    bool switchingDrawing_ = false;

    // Kept so the View menu can toggle them and the ribbon can raise the
    // layer manager on demand.
    QDockWidget* propertiesDock_ = nullptr;
    QDockWidget* layerDock_ = nullptr;
    QDockWidget* historyDock_ = nullptr;
    ProjectPanel* projectPanel_ = nullptr;
    QDockWidget* projectDock_ = nullptr;
    QPlainTextEdit* commandLog_ = nullptr;
    QLabel* coordinateLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QLabel* selectionLabel_ = nullptr;
    QLabel* statsLabel_ = nullptr;

    /// How many executor history lines have already been mirrored into the
    /// log panel, so each is appended exactly once.
    std::size_t loggedEntries_ = 0;
};

}  // namespace cad::app
