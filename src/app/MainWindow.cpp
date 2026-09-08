#include "MainWindow.hpp"
#include "AgentService.hpp"
#include "AgentSettingsDialog.hpp"
#include "ProjectDialog.hpp"
#include "ProjectIndex.hpp"

#include <array>
#include <functional>

#include <QProgressDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QElapsedTimer>
#include <QMenu>
#include <QMenuBar>
#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QMessageBox>
#include <QPageSize>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "Branding.hpp"
#include "CanvasWidget.hpp"
#include "PdfPages.hpp"
#include <QImageReader>
#include "CommandLine.hpp"
#include "RibbonIcons.hpp"
#include "Theme.hpp"
#include "QuickSelectDialog.hpp"
#include "PurgeDialog.hpp"
#include "PlotSetupDialog.hpp"
#include "ReferenceManagerDialog.hpp"
#include "SettingsDialog.hpp"
#include "ClipboardCommands.hpp"
#include "cad/command/MacroFile.hpp"
#include "ViewCommands.hpp"
#include "cad/db/Layer.hpp"
#include "cad/io/DxfWriter.hpp"
#include "cad/io/XrefLoader.hpp"

#include <cstdio>
#include <fstream>

#ifdef _WIN32
// Last, and narrowed: <windows.h> defines min, max and a great deal else that
// collides with ordinary C++ if it is allowed in ahead of the real headers.
// Guarded because the build already defines both project-wide.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cad::app {
namespace {

[[nodiscard]] QString formatCoordinate(double value) {
    return QString::number(value, 'f', 3);
}

/// Whether a process is still running.
///
/// Asking the operating system this is exactly the kind of thing the engine
/// must not do, so `RecoveryStore` takes it as a callback and this supplies it.
/// It decides whether a leftover auto-save belongs to a session that crashed or
/// to a second window that is running right now.
[[nodiscard]] bool isProcessRunning(std::uint64_t processId) {
#ifdef _WIN32
    const HANDLE handle =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (handle == nullptr) {
        // No such process, or one we may not query. Either way it is not
        // holding those files open.
        return false;
    }
    DWORD exitCode = 0;
    const bool running = GetExitCodeProcess(handle, &exitCode) != 0 && exitCode == STILL_ACTIVE;
    CloseHandle(handle);
    return running;
#else
    // Reporting "alive" suppresses the recovery offer, which is the safe way to
    // be wrong: it never invites a user to fork a drawing another window has
    // open. Recovery files remain on disk and in the log either way.
    (void)processId;
    return true;
#endif
}

}  // namespace

MainWindow::MainWindow() : translator_(io::makeDwgTranslator()) {
    // Before the first document: its registry has the user's aliases applied
    // as it is built, so they work from the first keystroke rather than after
    // some later refresh.
    loadConfig();

    // Recovery files go next to the configuration rather than beside the
    // drawing: a drawing may live on a read-only share or a network path that
    // is slow or gone, and auto-save must not be the thing that fails then.
    recovery_ = std::make_unique<io::RecoveryStore>(
        std::filesystem::path{QDir{QFileInfo{configPath()}.absolutePath()}
                                  .filePath("recovery")
                                  .toStdWString()},
        static_cast<std::uint64_t>(QCoreApplication::applicationPid()));

    // A drawing dragged from a folder and dropped anywhere on the window
    // opens in a new tab. Anywhere: the drop lands on whichever child is under
    // the pointer, and one that does not take drops hands it up to here.
    setAcceptDrops(true);

    buildUi();
    buildMenus();
    buildRibbon();
    onNew();
    buildSnapControl();
    applySettings();
    resize(1500, 950);

    // Queued rather than called here: the constructor runs before show(), so a
    // modal dialog raised from it appears over nothing, with no application
    // behind it to give it context. A zero-delay timer fires on the first turn
    // of the event loop, by which point the window is up — and still before the
    // user has done any work that recovering could displace.
    QTimer::singleShot(0, this, &MainWindow::offerRecovery);
    // Queued for a second reason: a macro that would not load is reported to
    // the console as well as the history, and whether this run has a console to
    // report to is decided after the window is constructed.
    QTimer::singleShot(0, this, &MainWindow::reportMacroProblems);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    // The canvas fills the window; the command line sits directly beneath it,
    // where a drafter's eye already is.
    auto* central = new QWidget{this};
    auto* layout = new QVBoxLayout{central};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    ribbon_ = new RibbonBar{central};
    canvas_ = new CanvasWidget{central};
    commandLine_ = new CommandLine{central};

    // Layout tabs sit between the canvas and the command line, where other CAD software
    // puts them. On a drawing set they are the primary navigation: the model is
    // the working area and the sheets are the deliverable.
    layoutTabs_ = new QTabBar{central};
    layoutTabs_->setExpanding(false);
    layoutTabs_->setDrawBase(false);
    layoutTabs_->setUsesScrollButtons(true);
    layoutTabs_->setElideMode(Qt::ElideRight);
    connect(layoutTabs_, &QTabBar::currentChanged, this, &MainWindow::onLayoutTabChanged);

    // File tabs go above the drawing area and below the ribbon, which is where
    // other CAD software puts them and, more to the point, where "which drawing am I in"
    // is answered before the eye reaches the geometry.
    fileTabs_ = new QTabBar{central};
    fileTabs_->setExpanding(false);
    fileTabs_->setDrawBase(false);
    fileTabs_->setUsesScrollButtons(true);
    fileTabs_->setElideMode(Qt::ElideMiddle);
    fileTabs_->setTabsClosable(true);
    fileTabs_->setMovable(true);
    connect(fileTabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (!switchingDrawing_) {
            activateDrawing(index);
        }
    });
    connect(fileTabs_, &QTabBar::tabCloseRequested, this,
            [this](int index) { static_cast<void>(closeDrawing(index)); });

    // The ribbon and the drawing tabs span the whole window, above the docks
    // rather than beside them.
    //
    // Left in the central widget they started at the inside edge of the
    // Properties panel, so the top-left corner of the application was a panel
    // caption with the ribbon beginning some way to its right -- two things
    // starting in two places where every other CAD application has one band of
    // chrome across the top. A top toolbar area is the one part of a
    // QMainWindow that sits above the dock areas.
    auto* topChrome = new QWidget{this};
    auto* topStack = new QVBoxLayout{topChrome};
    topStack->setContentsMargins(0, 0, 0, 0);
    topStack->setSpacing(0);
    ribbon_->setParent(topChrome);
    fileTabs_->setParent(topChrome);
    topStack->addWidget(ribbon_);
    topStack->addWidget(fileTabs_);

    auto* topBar = new QToolBar{tr("Ribbon"), this};
    topBar->setObjectName(QStringLiteral("ribbonBar"));
    topBar->setMovable(false);
    topBar->setFloatable(false);
    // Without this a right-click offers to hide the ribbon, and there would be
    // nothing left on screen to bring it back with.
    topBar->setContextMenuPolicy(Qt::PreventContextMenu);
    topBar->layout()->setContentsMargins(0, 0, 0, 0);
    // The padding and rule an ordinary toolbar gets are dropped by id in the
    // theme sheet; see the QToolBar#ribbonBar rule there.
    topBar->addWidget(topChrome);
    addToolBar(Qt::TopToolBarArea, topBar);

    layout->addWidget(canvas_, 1);
    layout->addWidget(layoutTabs_);
    layout->addWidget(commandLine_);
    setCentralWidget(central);

    // The command line's state is mirrored under the cursor: one buffer, two
    // views. It also holds the keyboard permanently, so typing works wherever
    // the pointer happens to be without any focus handover.
    connect(commandLine_, &CommandLine::inputChanged, this, &MainWindow::refreshDynamicInput);

    // Escape has to work wherever the focus happens to be, so it is watched for
    // at the application rather than waited for at this window.
    qApp->installEventFilter(this);

    connect(canvas_, &CanvasWidget::cursorMoved, this, &MainWindow::onCursorMoved);
    connect(canvas_, &CanvasWidget::viewChanged, this, &MainWindow::onViewChanged);
    connect(canvas_, &CanvasWidget::selectionChanged, this, &MainWindow::onSelectionChanged);
    connect(canvas_, &CanvasWidget::blockEditRequested, this, &MainWindow::onEditBlock);
    connect(canvas_, &CanvasWidget::contextMenuRequested, this,
            &MainWindow::showCanvasContextMenu);
    connect(canvas_, &CanvasWidget::snapOverrideRequested, this,
            &MainWindow::showSnapOverrideMenu);

    // Typing while a grip is held is direct distance entry, not a command.
    commandLine_->setDirectInput([this](const QString& text) {
        // The dimension boxes first, and they decline unless a field has been
        // locked with Tab. Both are up together during a grip edit, and the
        // grip handler reads a bare number as a distance -- so with a length
        // locked and an angle being typed, it would have taken the angle as a
        // distance and put the grip somewhere else entirely.
        return canvas_->supplyDimensionInput(text) || canvas_->supplyGripInput(text);
    });
    commandLine_->setGripMode([this] { return canvas_->gripModeName(); });
    connect(canvas_, &CanvasWidget::commandStateChanged, this,
            &MainWindow::onCommandStateChanged);
    connect(commandLine_, &CommandLine::commandStateChanged, this,
            &MainWindow::onCommandStateChanged);
    connect(commandLine_, &CommandLine::selectionCycleRequested, canvas_,
            &CanvasWidget::cycleHovered);

    // Say which of the overlapping objects is being offered, otherwise cycling
    // through a stack of identical-looking lines is guesswork.
    connect(canvas_, &CanvasWidget::hoverCycled, this, [this](int index, int total) {
        const QString description = canvas_->hoveredDescription();
        statusBar()->showMessage(
            tr("Cycling %1 of %2: %3  (Shift+Space)").arg(index).arg(total).arg(description),
            4000);
    });

    // Every panel can be closed. A drafter on a laptop needs the width, and a
    // palette that cannot be dismissed is a palette that is in the way; the
    // View menu is how each one comes back.
    const auto makeDock = [this](const QString& title, QWidget* content) {
        auto* dock = new QDockWidget{title, this};
        dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                          QDockWidget::DockWidgetClosable);
        content->setParent(dock);
        dock->setWidget(content);
        addDockWidget(Qt::LeftDockWidgetArea, dock);
        return dock;
    };

    properties_ = new PropertiesPanel{this};
    propertiesDock_ = makeDock(tr("Properties"), properties_);
    connect(properties_, &PropertiesPanel::propertyChanged, this, [this] {
        // A property edit can change what is drawn and what a layer looks
        // like, so both follow it.
        refreshLayerPanel();
        canvas_->update();
        updateWindowTitle();
    });

    layers_ = new LayerPanel{this};
    layerDock_ = makeDock(tr("Layers"), layers_);
    // Hidden by default: the layer controls a drafter reaches for are in the
    // ribbon now, and this is the manager behind them.
    layerDock_->hide();
    connect(layers_, &LayerPanel::layersChanged, this, [this] {
        refreshLayerPanel();
        canvas_->update();
        updateWindowTitle();
    });

    commandLog_ = new QPlainTextEdit{this};
    // A text view takes drops of its own -- it would paste the file's path as
    // text -- so it declines, and the drop reaches the window, which opens it.
    commandLog_->setAcceptDrops(false);
    commandLog_->setReadOnly(true);
    commandLog_->setMaximumBlockCount(1000);
    historyDock_ = makeDock(tr("History"), commandLog_);

    coordinateLabel_ = new QLabel{"0.000, 0.000", this};
    zoomLabel_ = new QLabel{"100%", this};
    selectionLabel_ = new QLabel{"", this};
    statsLabel_ = new QLabel{"", this};

    // The coordinate readout in a fixed-pitch face.
    //
    // In a proportional one every digit is a different width, so the numbers
    // shuffle sideways as the pointer moves and the readout is unreadable
    // while it is doing the one thing it exists for.
    QFont fixed{"Consolas"};
    fixed.setStyleHint(QFont::Monospace);
    fixed.setFixedPitch(true);
    fixed.setPointSizeF(font().pointSizeF());
    coordinateLabel_->setFont(fixed);
    zoomLabel_->setFont(fixed);
    coordinateLabel_->setMinimumWidth(180);
    coordinateLabel_->setToolTip(tr("Cursor position in drawing units"));
    zoomLabel_->setMinimumWidth(72);
    zoomLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    zoomLabel_->setToolTip(tr("Zoom, as a percentage of the drawing's own units"));
    statsLabel_->setStyleSheet(
        QString{"color: %1;"}.arg(currentTheme().textMuted.name()));

    // A rule between readouts, because four labels in a row with nothing
    // between them read as one sentence made of unrelated words.
    const auto separator = [this] {
        auto* line = new QFrame{this};
        line->setFrameShape(QFrame::VLine);
        line->setFixedWidth(1);
        line->setStyleSheet(QString{"background: %1; border: none;"}
                                .arg(currentTheme().panelBorder.name()));
        return line;
    };

    statusBar()->addWidget(coordinateLabel_);
    statusBar()->addWidget(separator());
    statusBar()->addWidget(zoomLabel_);
    statusBar()->addWidget(separator());
    statusBar()->addWidget(selectionLabel_);
    statusBar()->addPermanentWidget(statsLabel_);
    statusBar()->addPermanentWidget(separator());
    // The size grip is a Windows convention this window does not need: it is
    // resizable from every edge already, and the grip sits on top of the
    // right-most readout.
    statusBar()->setSizeGripEnabled(false);
    // Room at the right edge. Without it the last permanent widget is flush
    // against the frame and the Osnap button's menu arrow is clipped in half.
    statusBar()->setContentsMargins(0, 0, 8, 0);
}

void MainWindow::buildMenus() {
    // Built exactly once, in the constructor. It used to be rebuilt with every
    // document so the generated command entries could follow the registry —
    // which also called addToolBar again each time and left the previous
    // toolbar in place, so the window grew a duplicate row of buttons on the
    // first File > New. The commands live in the ribbon now, which rebuilds
    // itself properly, and the menu bar holds only what never changes.
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New"), QKeySequence::New, this, &MainWindow::onNew);
    fileMenu->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::onOpen);
    recentFilesMenu_ = fileMenu->addMenu(tr("Open &Recent"));
    recentFilesMenu_->setToolTipsVisible(true);
    rebuildRecentFilesMenu();
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::onSave);
    fileMenu->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, &MainWindow::onSaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Plot…"), QKeySequence::Print, this, &MainWindow::onPrint);
    fileMenu->addAction(tr("Plot Pre&view…"), this, &MainWindow::onPrintPreview);
    fileMenu->addAction(tr("&Export PDF…"), this, &MainWindow::onExportPdf);
    fileMenu->addAction(tr("Plot Set&up…"), this, &MainWindow::onPlotSetup);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    // A curated entry that *is* a registry command says so, and the generated
    // section skips what is already here. Without this the Edit menu offered
    // Undo twice and the Tools menu offered Quick Select twice -- the same
    // command written out by hand and then generated on top of it, which is
    // exactly the sort of doubling this window has too much of already.
    const auto owns = [](QAction* action, const char* command) {
        action->setData(QString::fromLatin1(command));
        return action;
    };

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    owns(editMenu->addAction(tr("&Undo"), QKeySequence::Undo, this, &MainWindow::onUndo),
         "UNDO");
    owns(editMenu->addAction(tr("&Redo"), QKeySequence::Redo, this, &MainWindow::onRedo),
         "REDO");
    editMenu->addSeparator();
    // The three shortcuts every application on the machine shares. A CAD
    // application that does not answer Ctrl+C feels broken long before anybody
    // reads its command list.
    owns(editMenu->addAction(tr("Cu&t"), QKeySequence::Cut, this,
                             [this] { runCommand("CUTCLIP"); }),
         "CUTCLIP");
    owns(editMenu->addAction(tr("&Copy"), QKeySequence::Copy, this,
                             [this] { runCommand("COPYCLIP"); }),
         "COPYCLIP");
    owns(editMenu->addAction(tr("&Paste"), QKeySequence::Paste, this,
                             [this] { runCommand("PASTECLIP"); }),
         "PASTECLIP");
    owns(editMenu->addAction(tr("Paste to &Original Coordinates"), this,
                             [this] { runCommand("PASTEORIG"); }),
         "PASTEORIG");

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    owns(viewMenu->addAction(tr("Zoom &Extents"), QKeySequence{"Ctrl+Shift+E"}, this,
                             &MainWindow::onZoomExtents),
         "ZOOMEXTENTS");
    viewMenu->addSeparator();

    // other CAD software's bindings for moving between sheets, and the only way to reach
    // a layout without taking a hand off the keyboard.
    viewMenu->addAction(tr("&Previous Layout"), QKeySequence{"Ctrl+PgUp"}, this,
                        [this] { stepLayout(-1); });
    viewMenu->addAction(tr("&Next Layout"), QKeySequence{"Ctrl+PgDown"}, this,
                        [this] { stepLayout(1); });
    viewMenu->addSeparator();

    // toggleViewAction() gives a checkable entry that already tracks the dock,
    // so closing a panel by its own button leaves the menu correct.
    auto* panels = viewMenu->addMenu(tr("&Panels"));
    panels->addAction(propertiesDock_->toggleViewAction());
    panels->addAction(layerDock_->toggleViewAction());
    panels->addAction(historyDock_->toggleViewAction());

    // The classic bar, for anybody who wants it back.
    //
    // other CAD software keeps the same switch under the same name, and for the same
    // reason: the menus are all on the ribbon's Menu button now, so the bar is
    // a preference rather than the only way in. Off by default because a row
    // that repeats the tab strip's own words is the thing this replaced.
    menuBarAction_ = viewMenu->addAction(tr("Menu &Bar"));
    menuBarAction_->setCheckable(true);
    menuBarAction_->setShortcut(QKeySequence{"Ctrl+Shift+M"});
    menuBarAction_->setStatusTip(tr("Show the classic menu bar above the ribbon"));
    connect(menuBarAction_, &QAction::toggled, this, [this](bool on) {
        menuBar()->setVisible(on);
        config_.setBoolean("ui", "menubar", on);
        config_.save(std::filesystem::path{configPath().toStdWString()});
    });

    // Customisation is only a feature if it is findable. A configuration file
    // nobody knows about is the same as no configuration file.
    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    owns(toolsMenu->addAction(tr("&Quick Select…"), this, &MainWindow::onQuickSelect),
         "QSELECT");
    toolsMenu->addSeparator();
    // Named for what it does. "AI Assistant" was the title of both this and the
    // AI command that actually asks the assistant for something, so the Tools
    // menu listed the same words twice for two different things.
    toolsMenu->addAction(tr("AI Assistant &Settings…"), this, &MainWindow::onAgentSettings);
    // The set the drawing belongs to: made here, kept beside the drawings,
    // never required.
    toolsMenu->addAction(tr("&Project…"), this, &MainWindow::onProject);
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("&Settings…"), QKeySequence::Preferences, this,
                         &MainWindow::onSettings);
    toolsMenu->addAction(tr("&Customize Aliases and Shortcuts…"), this,
                         &MainWindow::onOpenConfigFile);
    toolsMenu->addAction(tr("&Reload Configuration"), QKeySequence{"Ctrl+Shift+R"}, this,
                         &MainWindow::onReloadConfig);
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("&Discard Recovered Work…"), this,
                         &MainWindow::onDiscardRecoveryFiles);

    // Signals blocked: this is restoring the saved state, not a user changing
    // it, and letting it through would write the settings file on every start.
    const QSignalBlocker restoring{menuBarAction_};
    menuBarAction_->setChecked(config_.boolean("ui", "menubar", false));
    menuBar()->setVisible(menuBarAction_->isChecked());
    registerMenuShortcuts();
}

void MainWindow::registerMenuShortcuts() {
    // A hidden menu bar would take its accelerators down with it.
    //
    // Qt only matches a window-context shortcut while the widget holding the
    // action is visible, so with the bar off Ctrl+S would quietly stop saving
    // -- a silent failure on the one keystroke nobody checks. Adding the same
    // actions to the window itself keeps them live however the menus are
    // shown. Only the fixed entries carry accelerators; the generated command
    // ones are bound as application shortcuts in `applyShortcuts`.
    const std::function<void(QMenu*)> walk = [&](QMenu* menu) {
        for (QAction* action : menu->actions()) {
            if (QMenu* child = action->menu(); child != nullptr) {
                walk(child);
                continue;
            }
            if (!action->shortcut().isEmpty() && !actions().contains(action)) {
                addAction(action);
            }
        }
    };
    for (QAction* action : menuBar()->actions()) {
        if (QMenu* menu = action->menu(); menu != nullptr) {
            walk(menu);
        }
    }
}

void MainWindow::syncAppMenu() {
    // The Menu button's contents *are* the menu bar's menus -- the same QMenu
    // objects appearing in a second place, rather than a copy of them. A copy
    // would be a third list of what the application can do, and the whole point
    // of generating the menus was to stop keeping a second one.
    if (appMenu_ == nullptr) {
        return;
    }
    for (QAction* action : appMenu_->actions()) {
        appMenu_->removeAction(action);
    }
    for (QAction* action : menuBar()->actions()) {
        appMenu_->addAction(action);
    }
}

void MainWindow::stepLayout(int delta) {
    if (layoutTabs_->count() < 2) {
        return;
    }
    // Wraps, as other CAD software does: the tabs are a ring, and stopping at the end of
    // a nine-sheet set just means pressing the other key nine times.
    const int count = layoutTabs_->count();
    const int next = ((layoutTabs_->currentIndex() + delta) % count + count) % count;
    layoutTabs_->setCurrentIndex(next);
}

void MainWindow::buildRibbon() {
    // Everything above the drawing is one row.
    //
    // It used to be three: a menu bar, a strip of file buttons, and the
    // ribbon's tabs. Between them "Draw", "Annotate", "Modify" and "Insert"
    // appeared twice, one row above the other, which reads as two competing
    // navigations rather than as one -- and ninety pixels of a laptop screen
    // went to saying it. The menus are on a button, the file actions are icons
    // beside it, and the tabs follow on the same line.
    const Theme& theme = currentTheme();
    const auto quickButton = [&](const QString& icon, const QString& label,
                                 const QString& tip) {
        auto* button = new QToolButton{ribbon_};
        button->setObjectName(QStringLiteral("quickAccess"));
        button->setIcon(ribbonIcon(icon, label, 18, theme.text, theme.accent));
        button->setIconSize(QSize{18, 18});
        // The keystroke in the tip, not on the face. A row of six labelled
        // buttons is as wide as the tabs it now shares a line with; a row of
        // six pictures is not, and these six are the ones a drafter reaches
        // for often enough to learn by shape.
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        ribbon_->addQuickAccess(button);
        return button;
    };
    const auto quickSeparator = [this] {
        auto* rule = new QFrame{ribbon_};
        rule->setFrameShape(QFrame::VLine);
        rule->setFixedWidth(1);
        rule->setStyleSheet(QString{"background: %1; border: none; margin: 5px 3px;"}
                                .arg(currentTheme().panelBorder.name()));
        ribbon_->addQuickAccess(rule);
    };

    // The menus, on a button rather than on a bar of their own.
    //
    // Its contents are the menu bar's own menus -- the same QMenu objects, not
    // a second copy that could drift -- so everything the previous request put
    // on a menu is still exactly one press away, and `--dump-menus` still
    // answers for both.
    appMenu_ = new QMenu{this};
    auto* menuButton = quickButton(QStringLiteral("MENU"), tr("Menu"),
                                   tr("All menus — File, Edit, View, Draw, Annotate, "
                                      "Modify, Insert, Tools"));
    menuButton->setText(tr("Menu"));
    menuButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    menuButton->setPopupMode(QToolButton::InstantPopup);
    menuButton->setMenu(appMenu_);
    quickSeparator();

    const auto fileButton = [&](const QString& icon, const QString& label, const QString& tip,
                                void (MainWindow::*slot)()) {
        connect(quickButton(icon, label, tip), &QToolButton::clicked, this, slot);
    };
    fileButton(QStringLiteral("NEW"), tr("New"), tr("New drawing (Ctrl+N)"),
               &MainWindow::onNew);
    fileButton(QStringLiteral("OPEN"), tr("Open"), tr("Open a drawing (Ctrl+O)"),
               &MainWindow::onOpen);
    fileButton(QStringLiteral("SAVE"), tr("Save"), tr("Save (Ctrl+S)"), &MainWindow::onSave);
    fileButton(QStringLiteral("PLOT"), tr("Plot"), tr("Plot (Ctrl+P)"), &MainWindow::onPrint);
    quickSeparator();
    fileButton(QStringLiteral("UNDO"), tr("Undo"), tr("Undo (Ctrl+Z)"), &MainWindow::onUndo);
    fileButton(QStringLiteral("REDO"), tr("Redo"), tr("Redo (Ctrl+Y)"), &MainWindow::onRedo);
    quickSeparator();
    fileButton(QStringLiteral("ZOOMEXTENTS"), tr("Zoom Extents"),
               tr("Fit the drawing to the window (Ctrl+Shift+E)"), &MainWindow::onZoomExtents);
    quickSeparator();

    // Layers is a panel of controls rather than commands, so it cannot come
    // from the registry and is handed to the ribbon directly.
    layerRibbon_ = new LayerRibbon{ribbon_};
    ribbon_->setHomePanel(tr("Layers"), layerRibbon_);
    connect(layerRibbon_, &LayerRibbon::layersChanged, this, [this] {
        refreshLayerPanel();
        canvas_->update();
        updateWindowTitle();
    });
    connect(layerRibbon_, &LayerRibbon::manageRequested, this, [this] {
        layerDock_->show();
        layerDock_->raise();
    });

    connect(ribbon_, &RibbonBar::commandTriggered, this, [this](const QString& name) {
        runCommand(name.toStdString().c_str());
    });
}

void MainWindow::refreshRibbon() {
    ribbon_->rebuild(document_ == nullptr ? nullptr : &document_->commands());
}

void MainWindow::runCommand(const char* name) {
    if (document_ == nullptr) {
        return;
    }
    document_->executor().execute(name);
    document_->reindex();
    onCommandStateChanged();
    // A command started from the ribbon still takes its input from the
    // keyboard, so the command line keeps it.
    focusCommandLine();
}

void MainWindow::onNew() {
    // A new drawing opens beside the others rather than over them. Replacing
    // the open one was the old behaviour of a window that could only hold one.
    document_ = nullptr;
    currentPath_.clear();
    loggedEntries_ = 0;
    commandLog_->clear();
    addDrawing(makeDocument(), QString{});
    log(tr("New drawing."));
    reportPlugins();
    reportConfig();
    buildCommandMenus();
    onCommandStateChanged();
    focusCommandLine();
}

void MainWindow::focusCommandLine() {
    // The command line holds the keyboard permanently, which is what makes
    // "type anywhere" true. Without this the ribbon's tab bar takes it at
    // start-up and the first thing a drafter types — `l`, `m`, `c` — goes to a
    // tab strip. Clicks still reach the canvas, so nothing is lost by the
    // drawing area never holding focus.
    commandLine_->focusInput();
}

int MainWindow::activeDrawingIndex() const {
    for (std::size_t i = 0; i < drawings_.size(); ++i) {
        if (drawings_[i].document.get() == document_) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void MainWindow::addDrawing(std::unique_ptr<Document> document, const QString& path) {
    // The blank drawing a window starts with is a placeholder, not work. If it
    // is still untitled and untouched when a real drawing arrives, it goes --
    // otherwise every session that opens a file begins with a dead first tab
    // the user has to notice and close.
    if (!path.isEmpty() && drawings_.size() == 1 && drawings_.front().path.isEmpty() &&
        drawings_.front().document != nullptr && !drawings_.front().document->isModified()) {
        if (drawings_.front().document.get() == document_) {
            document_ = nullptr;
        }
        drawings_.clear();
    }

    OpenDrawing opened;
    opened.document = std::move(document);
    opened.path = path;
    drawings_.push_back(std::move(opened));
    refreshFileTabs();
    activateDrawing(static_cast<int>(drawings_.size()) - 1);
}

void MainWindow::activateDrawing(int index) {
    if (index < 0 || index >= static_cast<int>(drawings_.size())) {
        return;
    }
    if (drawings_[static_cast<std::size_t>(index)].document.get() == document_) {
        return;
    }

    // What the outgoing drawing looked like, so coming back to it is coming
    // back rather than starting again.
    if (const int previous = activeDrawingIndex(); previous >= 0) {
        OpenDrawing& leaving = drawings_[static_cast<std::size_t>(previous)];
        leaving.view = canvas_->viewport();
        leaving.hasView = true;
        leaving.log = commandLog_->toPlainText();
        leaving.loggedEntries = loggedEntries_;
        leaving.path = currentPath_;
    }

    OpenDrawing& entering = drawings_[static_cast<std::size_t>(index)];
    document_ = entering.document.get();
    currentPath_ = entering.path;
    loggedEntries_ = entering.loggedEntries;
    commandLog_->setPlainText(entering.log);
    commandLog_->moveCursor(QTextCursor::End);

    canvas_->setDocument(document_);
    commandLine_->setDocument(document_);
    setPanelDocument();

    // The registry belongs to the drawing, so the ribbon does too: a plugin
    // registered against one drawing must not offer its commands in another.
    refreshRibbon();
    refreshLayerPanel();
    refreshLayoutTabs();

    if (entering.hasView) {
        canvas_->setViewport(entering.view);
    } else {
        canvas_->zoomExtents();
    }

    if (orthoButton_ != nullptr) {
        // A drafting mode belongs to the drawing, so the button says what *this*
        // drawing has on rather than what the last one did.
        orthoButton_->setChecked(canvas_->orthoEnabled());
    }
    if (polarButton_ != nullptr) {
        polarButton_->setChecked(canvas_->polarEnabled());
    }
    if (lineweightButton_ != nullptr) {
        lineweightButton_->setChecked(document_ != nullptr &&
                                      document_->db().settings.displayLineweights);
    }
    if (trackingButton_ != nullptr) {
        trackingButton_->setChecked(canvas_->trackingEnabled());
    }
    refreshFileTabs();
    updateWindowTitle();
    // The menus follow the document, because the registry does: a drawing that
    // loaded a plugin has commands the last one did not.
    buildCommandMenus();
    onCommandStateChanged();
    focusCommandLine();
}

void MainWindow::refreshFileTabs() {
    // Rebuilding emits currentChanged per insertion, which would activate each
    // drawing in turn as a side effect of describing them.
    switchingDrawing_ = true;
    while (fileTabs_->count() > 0) {
        fileTabs_->removeTab(0);
    }
    for (const OpenDrawing& drawing : drawings_) {
        const QString name = drawing.path.isEmpty()
                                 ? tr("Untitled")
                                 : QFileInfo{drawing.path}.fileName();
        const bool modified = drawing.document != nullptr && drawing.document->isModified();
        const int index = fileTabs_->addTab(modified ? name + tr("*") : name);
        fileTabs_->setTabToolTip(index, drawing.path.isEmpty() ? name : drawing.path);
    }
    if (const int active = activeDrawingIndex(); active >= 0) {
        fileTabs_->setCurrentIndex(active);
    }
    // One drawing has nothing to switch between, and a single dead tab is
    // chrome rather than information -- the same rule the layout tabs follow.
    fileTabs_->setVisible(fileTabs_->count() > 1);
    switchingDrawing_ = false;
}

bool MainWindow::closeDrawing(int index) {
    if (index < 0 || index >= static_cast<int>(drawings_.size())) {
        return false;
    }
    // Asked about the drawing being closed, which means making it the one in
    // front of the user first: a dialog about unsaved changes has to be about
    // a drawing they can see.
    activateDrawing(index);
    if (!confirmDiscardChanges()) {
        return false;
    }

    drawings_.erase(drawings_.begin() + index);
    if (drawings_.empty()) {
        // Never no drawing at all. A window with no document has no command
        // line, no layers and no title, and every one of them would have to
        // learn to say so.
        document_ = nullptr;
        onNew();
        return true;
    }

    document_ = nullptr;
    refreshFileTabs();
    activateDrawing(std::min(index, static_cast<int>(drawings_.size()) - 1));
    return true;
}

std::unique_ptr<Document> MainWindow::makeDocument() {
    // Beside the executable, so the portable build stays a directory that can
    // be copied and run elsewhere with its plugins intact.
    const std::filesystem::path pluginDirectory =
        std::filesystem::path{QCoreApplication::applicationDirPath().toStdWString()} / "plugins";
    // The canvas outlives every document, so capturing it is safe.
    // `this` is safe to capture: the window outlives every document it makes.
    return std::make_unique<Document>(
        pluginDirectory, &config_, [this] {
            // Careful about ordering: the document being built is not yet the
            // window's, so the paste target is looked up when a paste actually
            // runs rather than captured here.
            std::vector<CommandDefinition> commands = clipboardCommands([this] {
                return document_ != nullptr ? &document_->activeSpace() : nullptr;
            });
            // Macros the user wrote, and SCRIPT. Loaded on every registry
            // rebuild -- which is every File > New and Open -- so editing a
            // macro and opening a drawing is enough to pick it up.
            const std::vector<std::filesystem::path> folders = macroFolders();
            cad::MacroLibrary library = cad::loadMacros(folders);
            macroProblems_.clear();
            for (const std::string& problem : library.problems) {
                macroProblems_.push_back(QString::fromStdString(problem));
            }
            for (CommandDefinition& macro : cad::macroCommands(std::move(library.macros))) {
                commands.push_back(std::move(macro));
            }
            for (CommandDefinition& script : cad::scriptCommands()) {
                commands.push_back(std::move(script));
            }

            std::vector<CommandDefinition> view = viewCommands(
                canvas_,
                [this](const std::string& name) {
                    if (document_ == nullptr || !document_->setActiveLayout(name)) {
                        return false;
                    }
                    refreshLayoutTabs();
                    canvas_->reload();
                    return true;
                },
                [this] { onQuickSelect(); }, [this] { onReferenceManager(); },
                [this] { onPurge(); },
                [this](const std::string& instruction) {
                    runAgent(QString::fromStdString(instruction));
                },
                attachHooks());
            commands.insert(commands.end(), std::make_move_iterator(view.begin()),
                            std::make_move_iterator(view.end()));
            return commands;
        });
}

std::vector<std::filesystem::path> MainWindow::macroFolders() {
    // Installed first, the user's second, so a file of the same name in the
    // user's folder shadows the shipped one. The shipped copy is never edited
    // in place: an update would overwrite the edit, and the original would be
    // gone with no way back.
    std::vector<std::filesystem::path> folders;
    folders.push_back(
        std::filesystem::path{QCoreApplication::applicationDirPath().toStdWString()} /
        "commands");
    folders.push_back(std::filesystem::path{
        QDir{settingsDirectory()}.filePath(QStringLiteral("commands")).toStdWString()});
    return folders;
}

void MainWindow::reportMacroProblems() {
    for (const QString& problem : macroProblems_) {
        log(tr("Macro not loaded -- %1").arg(problem));
    }
    if (document_ == nullptr) {
        return;
    }
    for (const std::string& problem : document_->commandProblems()) {
        log(tr("Command not registered -- %1").arg(QString::fromStdString(problem)));
    }
}

QString MainWindow::settingsDirectory() {
    // Per-user rather than beside the executable: settings belong to the
    // person, and a portable build on a shared machine must not write one
    // user's aliases into another's copy.
    //
    // GenericConfigLocation plus our own folder, not AppConfigLocation, which
    // nests <organization>/<application> and so lands in a Slate folder inside
    // a Slate folder. A path a user is told to open should be one they can also
    // find on their own.
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir{directory}.filePath(kDisplayName);
}

QString MainWindow::configPath() {
    return QDir{settingsDirectory()}.filePath(QString{"%1.ini"}.arg(kShortName));
}

void MainWindow::adoptPreviousSettings() {
    // The rename must not cost anyone their settings, and it must not cost
    // anyone their unsaved work.
    //
    // Renaming an application renames the folder it keeps both of those in. A
    // user who had recovery files waiting from a crash would find a fresh,
    // empty application looking in a folder that had never existed — the work
    // still on disk, and nothing left that knows to look for it. Moving the old
    // folder across is a one-line problem here and an unrecoverable one for
    // them.
    const QString current = settingsDirectory();
    if (QFileInfo::exists(current)) {
        return;
    }
    const QString previous =
        QDir{QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)}.filePath(
            kPreviousProductName);
    if (!QFileInfo::exists(previous)) {
        return;
    }

    // Copied, not moved. If anything about this goes wrong the user still has
    // the original, and an application that deletes the previous version's data
    // during an upgrade has no way to apologise.
    if (!copyDirectory(previous, current)) {
        return;
    }
    // The configuration file is named after the application too.
    const QString oldName =
        QDir{current}.filePath(QString{"%1.ini"}.arg(kPreviousProductName).toLower());
    if (QFileInfo::exists(oldName) && !QFileInfo::exists(configPath())) {
        QFile::rename(oldName, configPath());
    }
    pendingMigrationNotice_ = true;
}

bool MainWindow::copyDirectory(const QString& from, const QString& to) {
    QDir source{from};
    if (!source.exists() || !QDir{}.mkpath(to)) {
        return false;
    }
    for (const QFileInfo& entry :
         source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString target = QDir{to}.filePath(entry.fileName());
        const bool copied = entry.isDir() ? copyDirectory(entry.absoluteFilePath(), target)
                                          : QFile::copy(entry.absoluteFilePath(), target);
        if (!copied) {
            return false;
        }
    }
    return true;
}

void MainWindow::loadConfig() {
    adoptPreviousSettings();
    const QString path = configPath();
    // Written on first run rather than left absent, because a file the user is
    // meant to hand-edit is undiscoverable if it does not exist and is not
    // mentioned anywhere.
    if (!QFileInfo::exists(path)) {
        ConfigFile starter = ConfigFile::parse(defaultConfigText());
        starter.save(std::filesystem::path{path.toStdWString()});
    }
    config_ = ConfigFile::load(std::filesystem::path{path.toStdWString()});
}

void MainWindow::reportConfig() {
    if (document_ == nullptr) {
        return;
    }
    if (pendingMigrationNotice_) {
        pendingMigrationNotice_ = false;
        log(tr("Settings and recovery files were brought across from %1.")
                .arg(kPreviousProductName));
    }
    const ConfigApplyResult& result = document_->configResult();
    if (result.applied > 0) {
        log(tr("%1 alias(es) from your configuration.").arg(result.applied));
    }
    // A misspelled alias silently doing nothing is the worst outcome: the user
    // types it, gets no command, and has no way to learn why.
    for (const std::string& problem : result.problems) {
        log(tr("! %1").arg(QString::fromStdString(problem)));
    }
}

void MainWindow::buildCommandMenus() {
    if (document_ == nullptr) {
        return;
    }
    // Generated from the registry rather than written out.
    //
    // A hand-written menu is a second list of what the application can do, and
    // the moment a command is added it is wrong. Built from the registry, a new
    // command -- including one a plugin brought -- is on a menu the first time
    // it is loaded, and a removed one leaves. `menuCoverage` checks the promise
    // holds rather than trusting that it does.
    for (QMenu* menu : commandMenus_) {
        menuBar()->removeAction(menu->menuAction());
        menu->deleteLater();
    }
    commandMenus_.clear();
    for (const auto& [name, actions] : appendedCommandActions_) {
        for (QAction* action : actions) {
            delete action;
        }
    }
    appendedCommandActions_.clear();

    // What the hand-written menus already offer, so the generated section does
    // not offer it a second time. Read *after* the previous generation has been
    // torn down, so only the curated entries are left to find.
    const QStringList curated = menuCoverage();

    const std::map<std::string, std::vector<const CommandDefinition*>> byCategory =
        document_->commands().byCategory();

    // Which menu a category lands on, and in what order the menus appear.
    //
    // Categories are how commands are grouped for the ribbon, and there are
    // more of them than a menu bar wants: a two-item Block menu beside a
    // one-item Insert menu is a worse place to look than one Insert menu with
    // three things on it. Anything not named here gets its own menu, which is
    // what a plugin's category should do.
    const std::vector<std::pair<const char*, const char*>> homes{
        {"Draw", "&Draw"},         {"Annotate", "&Annotate"}, {"Dimension", "&Annotate"},
        {"Modify", "&Modify"},     {"Insert", "&Insert"},     {"Block", "&Insert"},
        {"View", "&View"},         {"Tools", "&Tools"},       {"File", "&File"},
    };
    const auto homeFor = [&](const std::string& category) {
        for (const auto& [from, to] : homes) {
            if (category == from) {
                return QString::fromLatin1(to);
            }
        }
        return QString::fromStdString(category);
    };

    // Ordered, and gathered so that two categories sharing a menu are added to
    // it together rather than with the rest of the bar in between.
    struct Section {
        std::string category;
        std::vector<const CommandDefinition*> commands;
    };
    std::vector<QString> order;
    std::map<QString, std::vector<Section>> grouped;
    for (const auto& [category, commands] : byCategory) {
        std::vector<const CommandDefinition*> fresh;
        for (const CommandDefinition* command : commands) {
            if (command != nullptr &&
                !curated.contains(QString::fromStdString(command->name))) {
                fresh.push_back(command);
            }
        }
        if (fresh.empty()) {
            continue;  // Every command here is already on a menu by hand.
        }
        const QString home = homeFor(category);
        if (grouped.find(home) == grouped.end()) {
            order.push_back(home);
        }
        grouped[home].push_back(Section{category, std::move(fresh)});
    }
    const std::vector<QString> preferred{QStringLiteral("&Draw"), QStringLiteral("&Annotate"),
                                         QStringLiteral("&Modify"), QStringLiteral("&Insert")};
    std::stable_sort(order.begin(), order.end(), [&](const QString& a, const QString& b) {
        const auto rank = [&](const QString& title) {
            const auto at = std::find(preferred.begin(), preferred.end(), title);
            return at == preferred.end() ? preferred.size()
                                         : static_cast<std::size_t>(at - preferred.begin());
        };
        return rank(a) < rank(b);
    });

    for (const QString& title : order) {
        const std::vector<Section>& sections = grouped[title];

        // A category that already has a menu of its own -- View, Tools -- gets
        // its commands appended to it rather than a second menu with the same
        // name beside it.
        const QString bare = QString{title}.remove(QLatin1Char{'&'});
        QMenu* target = nullptr;
        QAction* toolsAction = nullptr;
        for (QAction* action : menuBar()->actions()) {
            QMenu* menu = action->menu();
            if (menu == nullptr) {
                continue;
            }
            const QString name = menu->title().remove(QLatin1Char{'&'});
            if (name.compare(bare, Qt::CaseInsensitive) == 0) {
                target = menu;
            }
            if (name.compare(QLatin1String{"Tools"}, Qt::CaseInsensitive) == 0) {
                toolsAction = action;
            }
        }

        std::vector<QAction*> added;
        const bool existing = target != nullptr;
        if (existing) {
            added.push_back(target->addSeparator());
        } else {
            // Before Tools, so the drawing menus sit where a drafter expects
            // them and the application's own settings stay on the right.
            target = new QMenu{title, this};
            if (toolsAction != nullptr) {
                menuBar()->insertMenu(toolsAction, target);
            } else {
                menuBar()->addMenu(target);
            }
            commandMenus_.push_back(target);
        }

        for (const Section& section : sections) {
            // A category folded into somebody else's menu gets a submenu of its
            // own, because its titles were written to be read under a heading:
            // "Aligned" and "Angular" mean something under Dimension and very
            // little in a flat list under Annotate. A category on its own menu
            // needs no such heading -- a Draw submenu inside Draw says nothing.
            QMenu* into = target;
            const bool ownMenu =
                QString::fromStdString(section.category).compare(bare, Qt::CaseInsensitive) == 0;
            if (!ownMenu && sections.size() > 1) {
                into = target->addMenu(QString::fromStdString(section.category));
                if (existing) {
                    added.push_back(into->menuAction());
                }
            }

            for (const CommandDefinition* command : section.commands) {
                if (command == nullptr) {
                    continue;
                }
                const QString label = QString::fromStdString(
                    command->title.empty() ? command->name : command->title);
                const std::string name = command->name;
                QAction* action = into->addAction(label, this, [this, name] {
                    runCommand(name.c_str());
                });
                // The command's own name in the tip, because that is what gets
                // typed once somebody has found it here twice.
                action->setStatusTip(QString::fromStdString(command->description));
                action->setToolTip(QString::fromStdString(command->name));
                action->setData(QString::fromStdString(command->name));
                if (existing && into == target) {
                    added.push_back(action);
                }
            }
        }
        if (!added.empty()) {
            appendedCommandActions_[target] = std::move(added);
        }
    }

    syncAppMenu();
    applyShortcuts();
}

QStringList MainWindow::menuCoverage() const {
    // Every command name reachable from the menu bar, however deeply nested.
    QStringList found;
    const std::function<void(const QMenu*)> walk = [&](const QMenu* menu) {
        for (const QAction* action : menu->actions()) {
            if (const QMenu* child = action->menu(); child != nullptr) {
                walk(child);
                continue;
            }
            const QString name = action->data().toString();
            if (!name.isEmpty()) {
                found << name;
            }
        }
    };
    for (const QAction* action : menuBar()->actions()) {
        if (const QMenu* menu = action->menu(); menu != nullptr) {
            walk(menu);
        }
    }
    found.sort();
    found.removeDuplicates();
    return found;
}

void MainWindow::dumpMenus() {
    // Printed as "menu > submenu > item [COMMAND]" so a scripted run can check
    // both that a command is reachable and where it was put.
    const std::function<void(const QMenu*, const QString&)> walk =
        [&](const QMenu* menu, const QString& path) {
            for (const QAction* action : menu->actions()) {
                if (action->isSeparator()) {
                    continue;
                }
                const QString label = action->text().remove(QLatin1Char{'&'});
                if (const QMenu* child = action->menu(); child != nullptr) {
                    walk(child, path + label + QStringLiteral(" > "));
                    continue;
                }
                const QString name = action->data().toString();
                log(QStringLiteral("MENU %1%2%3")
                        .arg(path, label,
                             name.isEmpty() ? QString{} : QStringLiteral(" [") + name + "]"));
            }
        };
    for (const QAction* action : menuBar()->actions()) {
        if (const QMenu* menu = action->menu(); menu != nullptr) {
            walk(menu, menu->title().remove(QLatin1Char{'&'}) + QStringLiteral(" > "));
        }
    }

    // The check itself, so a scripted run does not have to reimplement it.
    QStringList missing;
    if (document_ != nullptr) {
        const QStringList covered = menuCoverage();
        for (const auto& [category, commands] : document_->commands().byCategory()) {
            for (const CommandDefinition* command : commands) {
                if (command != nullptr &&
                    !covered.contains(QString::fromStdString(command->name))) {
                    missing << QString::fromStdString(command->name);
                }
            }
        }
    }
    log(missing.isEmpty()
            ? QStringLiteral("MENU-COVERAGE complete")
            : QStringLiteral("MENU-COVERAGE missing: %1").arg(missing.join(QStringLiteral(", "))));

    // Where the menus are actually *shown*. The bar above holds them all, but
    // it is hidden by default, so a run that only checked the bar would pass
    // with nothing reachable by hand. This reports what the ribbon's Menu
    // button opens, and whether the classic bar is up.
    QStringList onButton;
    if (appMenu_ != nullptr) {
        for (const QAction* action : appMenu_->actions()) {
            onButton << action->text().remove(QLatin1Char{'&'});
        }
    }
    log(QStringLiteral("MENU-BUTTON %1").arg(onButton.join(QStringLiteral(", "))));
    // isHidden, not isVisible: a scripted run's window is never shown, so
    // isVisible is false for everything in it and would report "hidden" even
    // with the bar switched on.
    QStringList tabs;
    for (int i = 0; i < fileTabs_->count(); ++i) {
        tabs << fileTabs_->tabText(i);
    }
    log(QStringLiteral("FILE-TABS %1: %2").arg(fileTabs_->count()).arg(tabs.join(", ")));
    log(QStringLiteral("MENU-BAR %1").arg(menuBar()->isHidden() ? QStringLiteral("hidden")
                                                                : QStringLiteral("shown")));
}

void MainWindow::applyShortcuts() {
    // Rebuilt from scratch: reloading a configuration must not leave the
    // previous bindings live alongside the new ones.
    for (QShortcut* shortcut : shortcuts_) {
        delete shortcut;
    }
    shortcuts_.clear();
    if (document_ == nullptr) {
        return;
    }

    ConfigApplyResult problems;
    const std::vector<ShortcutBinding> bindings =
        readShortcuts(document_->commands(), config_, problems);

    for (const ShortcutBinding& binding : bindings) {
        const QKeySequence sequence{QString::fromStdString(binding.keys)};
        if (sequence.isEmpty()) {
            log(tr("! Shortcut \"%1\" is not a key sequence I understand.")
                    .arg(QString::fromStdString(binding.keys)));
            continue;
        }
        auto* shortcut = new QShortcut{sequence, this};
        // Application-wide: a shortcut typed while the pointer is over a panel
        // should still run, and the command line holds the keyboard at all
        // times so a widget-scoped shortcut would almost never fire.
        shortcut->setContext(Qt::ApplicationShortcut);
        const std::string id = binding.commandId;
        connect(shortcut, &QShortcut::activated, this, [this, id] {
            if (document_ != nullptr) {
                if (const CommandDefinition* command = document_->commands().get(id)) {
                    runCommand(command->name.c_str());
                }
            }
        });
        shortcuts_.push_back(shortcut);
    }

    for (const std::string& problem : problems.problems) {
        log(tr("! %1").arg(QString::fromStdString(problem)));
    }
    if (!shortcuts_.empty()) {
        log(tr("%1 keyboard shortcut(s) bound.").arg(shortcuts_.size()));
    }
}

void MainWindow::onReloadConfig() {
    loadConfig();
    // Aliases live in the registry, which belongs to the document, so the
    // document has to be told. Rebuilding the whole document would throw away
    // the drawing; re-applying to the existing registry would leave previously
    // applied aliases in place. Reapplying over the top is correct because an
    // alias is a name-to-command mapping, and re-adding is idempotent.
    if (document_ != nullptr) {
        const ConfigApplyResult result = applyAliases(document_->commands(), config_);
        log(tr("Configuration reloaded: %1 alias(es).").arg(result.applied));
        for (const std::string& problem : result.problems) {
            log(tr("! %1").arg(QString::fromStdString(problem)));
        }
    }
    applyShortcuts();
    // A hand-edited file must reach the same places the dialog does.
    applySettings();
}

void MainWindow::onOpenConfigFile() {
    const QString path = configPath();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::information(this, tr("Configuration"),
                                 tr("Your configuration file is at:\n\n%1").arg(path));
    }
}

void MainWindow::applySettings() {
    // Everything the dialog can change is applied from here, so the dialog, a
    // hand-edited file and a configuration reload all take the same path.
    canvas_->setAperturePixels(
        static_cast<double>(config_.integer("drafting", "aperture", 10)));
    canvas_->setCrosshairPercent(
        static_cast<double>(config_.integer("drafting", "crosshair", 5)));
    canvas_->setTextGreekingPixels(
        static_cast<double>(config_.integer("drafting", "text.greeking", 3)));
    canvas_->setShowImageFrames(config_.boolean("drafting", "image.frames", false));
    canvas_->setRolloverHighlight(config_.boolean("drafting", "rollover", true));
    configureAutoSave();
    canvas_->update();
}

void MainWindow::onSettings() {
    if (nonInteractive_) {
        return;
    }
    SettingsDialog dialog{config_, [this] {
                              config_.save(std::filesystem::path{configPath().toStdWString()});
                              applySettings();
                              log(tr("Settings saved."));
                          },
                          this};
    connect(&dialog, &SettingsDialog::openConfigFileRequested, this,
            &MainWindow::onOpenConfigFile);
    dialog.exec();
}

void MainWindow::onQuickSelect() {
    if (document_ == nullptr) {
        return;
    }
    SelectionFilter filter;
    if (nonInteractive_) {
        // No dialog to answer. Reported rather than silently doing nothing, and
        // it keeps a scripted run from blocking for ever on a modal window.
        log(tr("QSELECT needs the dialog, which is unavailable in a scripted run."));
        return;
    }
    QuickSelectDialog dialog{*document_, this};
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    filter = dialog.filter();
    // Narrowing an existing selection is what makes successive queries useful:
    // "every dimension", then "of those, the ones on the demolition layer".
    if (filter.mode == SelectionFilter::Mode::Remove && !document_->selection().empty()) {
        filter.within = document_->selection();
    }

    const std::vector<Handle> matches =
        findByFilter(document_->db(), document_->db().modelSpace(), filter);
    const std::vector<Handle> result =
        applySelectionMode(document_->selection(), matches, filter.mode);

    document_->setSelection(result);
    onSelectionChanged();
    canvas_->update();
    log(tr("Quick select matched %1 object(s); %2 now selected.")
            .arg(matches.size())
            .arg(result.size()));
}

PlotRequest MainWindow::currentPlotRequest() const {
    PlotRequest request;
    if (document_ != nullptr && document_->inPaperSpace()) {
        request.layout = &document_->activeLayout();
    }
    // What Plot Setup chose: the area, the scale and the ink. Monochrome by
    // default: a drawing authored against a dark background is largely
    // white, and white ink is no ink.
    const PlotSetup setup = PlotSetup::load(config_);
    request.monochrome = setup.monochrome;
    if (setup.area == PlotSetup::Area::Window && !setup.window.isEmpty()) {
        request.window = setup.window;
    }
    request.paperPerUnit = setup.paperPerUnit;
    return request;
}

void MainWindow::onPlotSetup() {
    PlotSetupDialog dialog{PlotSetup::load(config_),
                           document_ != nullptr && document_->inPaperSpace(), this};
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    dialog.setup().save(config_);
    config_.save(std::filesystem::path{configPath().toStdWString()});
    log(tr("Plot setup saved."));
}

void MainWindow::renderPlot(QPrinter& printer) {
    if (document_ == nullptr) {
        return;
    }
    QPainter painter;
    if (!painter.begin(&printer)) {
        log(tr("! Could not start the plot device."));
        return;
    }

    PlotRequest request = currentPlotRequest();
    // A printer's pixels are far smaller than a screen's, so scale line widths
    // and the text cull with the device or the plot comes out ghostly.
    request.lineWidthScale = std::max(1.0, printer.logicalDpiX() / 96.0);

    // The *printable* area, not the full page: plotting into the physical
    // margin is how a title block loses its border.
    const QRectF page = printer.pageLayout().paintRectPixels(printer.resolution());
    std::size_t omitted = 0;
    if (!paintPlot(painter, *document_, QRectF{QPointF{0, 0}, page.size()}, request, &omitted)) {
        log(tr("! Nothing to plot."));
    }
    painter.end();
    reportOmittedContent(omitted);
}

void MainWindow::reportOmittedContent(std::size_t count) {
    if (count == 0) {
        return;
    }
    // An attachment this application cannot draw prints as nothing, which is
    // right — a placeholder outline is a mark that is not in the drawing. It is
    // also invisible, so the plot has to say so out loud.
    log(tr("  ! %1 attached item(s) could not be drawn and were left blank. "
           "Use Plot Preview to see where.")
            .arg(count));
}

void MainWindow::onPrint() {
    if (document_ == nullptr) {
        return;
    }
    QPrinter printer{QPrinter::HighResolution};
    QPrintDialog dialog{&printer, this};
    dialog.setWindowTitle(tr("Plot — %1").arg(plotDescription(*document_, currentPlotRequest())));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    renderPlot(printer);
    log(tr("Plotted %1").arg(plotDescription(*document_, currentPlotRequest())));
}

void MainWindow::onPrintPreview() {
    if (document_ == nullptr) {
        return;
    }
    QPrinter printer{QPrinter::HighResolution};
    QPrintPreviewDialog preview{&printer, this};
    preview.setWindowTitle(tr("Plot Preview — %1")
                               .arg(plotDescription(*document_, currentPlotRequest())));
    // The preview paints through renderPlot, the same call the printer uses.
    // A preview drawn by separate code is a preview that can lie.
    connect(&preview, &QPrintPreviewDialog::paintRequested, this,
            [this](QPrinter* target) { renderPlot(*target); });
    preview.resize(1000, 800);
    preview.exec();
}

void MainWindow::onExportPdf() {
    if (document_ == nullptr) {
        return;
    }
    const QString suggested =
        currentPath_.isEmpty() ? QStringLiteral("drawing.pdf")
                               : QFileInfo{currentPath_}.completeBaseName() + ".pdf";
    const QString path = QFileDialog::getSaveFileName(this, tr("Export PDF"), suggested,
                                                      tr("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }
    exportPdfTo(path);
}

bool MainWindow::plotToImage(const QString& path, int widthPixels) {
    if (document_ == nullptr) {
        return false;
    }

    // Page proportions come from the sheet, so the image is the shape the paper
    // would be rather than an arbitrary rectangle the drawing is squeezed into.
    double aspect = 297.0 / 210.0;
    if (document_->inPaperSpace()) {
        const Vec3 size = document_->activeLayout().plot.sheetSize();
        if (size.x > 0.0 && size.y > 0.0) {
            aspect = size.y / size.x;
        }
    } else if (const Box3 extents = document_->db().modelExtents();
               !extents.isEmpty() && extents.width() > 0.0) {
        aspect = extents.height() / extents.width();
    }

    QImage image{widthPixels, std::max(1, static_cast<int>(widthPixels * aspect)),
                 QImage::Format_RGB32};
    image.fill(Qt::white);

    QPainter painter{&image};
    PlotRequest request = currentPlotRequest();
    request.lineWidthScale = std::max(1.0, widthPixels / 1000.0);
    // An image of the plot is something to look at rather than something to
    // issue, so it is a preview: the sheet outline and the empty-content
    // outlines both belong in it.
    request.showSheetOutline = true;
    request.showContentPlaceholders = true;

    const bool drawn =
        paintPlot(painter, *document_,
                  QRectF{0.0, 0.0, static_cast<qreal>(image.width()),
                         static_cast<qreal>(image.height())},
                  request);
    painter.end();
    return drawn && image.save(path);
}

bool MainWindow::exportPdfTo(const QString& path) {
    if (document_ == nullptr) {
        return false;
    }

    QPrinter printer{QPrinter::HighResolution};
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);

    // Match the page to the sheet, so an A1 layout does not come out letterbox
    // on A4 with everything shrunk into the middle.
    if (document_->inPaperSpace()) {
        const PlotSettings& plot = document_->activeLayout().plot;
        const bool landscape = plot.sheetSize().x > plot.sheetSize().y;
        printer.setPageOrientation(landscape ? QPageLayout::Landscape : QPageLayout::Portrait);
        // Stored in millimetres whatever the drawing's units, which is exactly
        // what QPageSize wants.
        const double widthMm = landscape ? std::max(plot.paperWidth, plot.paperHeight)
                                         : std::min(plot.paperWidth, plot.paperHeight);
        const double heightMm = landscape ? std::min(plot.paperWidth, plot.paperHeight)
                                          : std::max(plot.paperWidth, plot.paperHeight);
        printer.setPageSize(QPageSize{QSizeF{widthMm, heightMm}, QPageSize::Millimeter});
    }
    printer.setPageMargins(QMarginsF{0, 0, 0, 0});

    renderPlot(printer);
    log(tr("Exported %1 to %2")
            .arg(plotDescription(*document_, currentPlotRequest()), QFileInfo{path}.fileName()));
    return QFileInfo::exists(path);
}

void MainWindow::buildSnapControl() {
    // OSMODE, the same bitmask other CAD software stores, so a value carried over from a
    // drawing or a colleague's settings means the same thing here.
    const auto saved = static_cast<std::uint32_t>(
        config_.integer("drafting", "osmode", static_cast<long long>(kDefaultSnapModes)));

    snapButton_ = new QToolButton{this};
    snapButton_->setText(tr("Osnap"));
    snapButton_->setToolTip(tr("Object snaps. Click to toggle, or choose which are running."));
    snapButton_->setCheckable(true);
    snapButton_->setAutoRaise(true);
    snapButton_->setCursor(Qt::PointingHandCursor);
    snapButton_->setFocusPolicy(Qt::NoFocus);
    snapButton_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* menu = new QMenu{snapButton_};
    for (const SnapMode mode : allSnapModes()) {
        // Clear is not a snap the user picks; it is the absence of them, which
        // the button itself expresses.
        if (mode == SnapMode::Clear || mode == SnapMode::None) {
            continue;
        }
        QAction* action = menu->addAction(QString::fromUtf8(snapModeName(mode).data(),
                                                            static_cast<int>(
                                                                snapModeName(mode).size())));
        action->setCheckable(true);
        connect(action, &QAction::toggled, this, [this, mode](bool on) {
            std::uint32_t modes = canvas_->snapModes();
            modes = on ? (modes | toMask(mode)) : (modes & ~toMask(mode));
            setSnapModes(modes);
        });
        snapActions_.emplace_back(mode, action);
    }

    menu->addSeparator();
    menu->addAction(tr("Select All"), this, [this] {
        std::uint32_t modes = 0;
        for (const auto& [mode, action] : snapActions_) {
            modes |= toMask(mode);
        }
        setSnapModes(modes);
    });
    menu->addAction(tr("Clear All"), this, [this] { setSnapModes(0); });
    menu->addAction(tr("Restore Defaults"), this, [this] { setSnapModes(kDefaultSnapModes); });
    snapButton_->setMenu(menu);

    connect(snapButton_, &QToolButton::clicked, this, [this](bool checked) {
        // The master switch keeps the chosen set rather than discarding it, so
        // turning snapping off for one pick does not cost the configuration.
        setSnapModes(checked ? savedSnapModes_ : 0);
    });

    // ORTHO beside it, because the two are read together: a drafter glances at
    // this strip to find out why the line will not go where they are pointing.
    orthoButton_ = new QToolButton{this};
    orthoButton_->setText(tr("Ortho"));
    orthoButton_->setToolTip(tr("Constrain points to horizontal and vertical (F8)"));
    orthoButton_->setCheckable(true);
    orthoButton_->setAutoRaise(true);
    orthoButton_->setCursor(Qt::PointingHandCursor);
    orthoButton_->setFocusPolicy(Qt::NoFocus);
    connect(orthoButton_, &QToolButton::clicked, this, &MainWindow::toggleOrtho);
    statusBar()->addPermanentWidget(orthoButton_);

    polarButton_ = new QToolButton{this};
    polarButton_->setText(tr("Polar"));
    polarButton_->setToolTip(tr("Pull points onto regular angles (F10)"));
    polarButton_->setCheckable(true);
    polarButton_->setAutoRaise(true);
    polarButton_->setCursor(Qt::PointingHandCursor);
    polarButton_->setFocusPolicy(Qt::NoFocus);
    connect(polarButton_, &QToolButton::clicked, this, &MainWindow::togglePolar);
    statusBar()->addPermanentWidget(polarButton_);

    trackingButton_ = new QToolButton{this};
    trackingButton_->setText(tr("Otrack"));
    trackingButton_->setToolTip(
        tr("Align to snap points the pointer has rested on (F11)"));
    trackingButton_->setCheckable(true);
    trackingButton_->setAutoRaise(true);
    trackingButton_->setCursor(Qt::PointingHandCursor);
    trackingButton_->setFocusPolicy(Qt::NoFocus);
    connect(trackingButton_, &QToolButton::clicked, this, &MainWindow::toggleTracking);
    statusBar()->addPermanentWidget(trackingButton_);

    // Lineweights are a plotting property; on screen they are a choice, and
    // one that lives with the drawing, as LWDISPLAY does in the file.
    lineweightButton_ = new QToolButton{this};
    lineweightButton_->setText(tr("LWT"));
    lineweightButton_->setToolTip(tr("Show lineweights at their width on screen"));
    lineweightButton_->setCheckable(true);
    lineweightButton_->setAutoRaise(true);
    lineweightButton_->setCursor(Qt::PointingHandCursor);
    lineweightButton_->setFocusPolicy(Qt::NoFocus);
    connect(lineweightButton_, &QToolButton::clicked, this, &MainWindow::toggleLineweights);
    statusBar()->addPermanentWidget(lineweightButton_);

    statusBar()->addPermanentWidget(snapButton_);

    // A drafting mode that is on has to look on. Auto-raised buttons in a dark
    // theme show almost nothing when checked, so the strip read the same with
    // ortho on as with it off -- and this strip exists precisely for the
    // drafter wondering why the line will not go where they are pointing.
    // other CAD software lights its own the same way.
    const QString toggleStyle = QString{R"(
        QToolButton:checked {
            background: %1;
            color: white;
            border-radius: 3px;
        }
        QToolButton:hover:!checked { background: %2; border-radius: 3px; }
    )"}
                                    .arg(currentTheme().accent.name(),
                                         currentTheme().panel.name());
    for (QToolButton* button : {orthoButton_, polarButton_, trackingButton_, snapButton_}) {
        if (button != nullptr) {
            button->setStyleSheet(toggleStyle);
        }
    }

    setSnapModes(saved);
}

void MainWindow::setSnapModes(std::uint32_t modes) {
    canvas_->setSnapModes(modes);
    if (modes != 0) {
        savedSnapModes_ = modes;
    }

    {
        // Blocked: setChecked would otherwise re-enter through the toggled
        // handler and rewrite the mask being applied.
        const QSignalBlocker blockButton{snapButton_};
        snapButton_->setChecked(modes != 0);
        for (const auto& [mode, action] : snapActions_) {
            const QSignalBlocker blockAction{action};
            action->setChecked(hasMode(modes, mode));
        }
    }

    config_.set("drafting", "osmode", std::to_string(modes));
    config_.save(std::filesystem::path{configPath().toStdWString()});
    canvas_->update();
}

void MainWindow::configureAutoSave() {
    if (autoSaveTimer_ == nullptr) {
        autoSaveTimer_ = new QTimer{this};
        connect(autoSaveTimer_, &QTimer::timeout, this, &MainWindow::autoSave);
    }

    const bool enabled = config_.boolean("files", "autosave.enabled", true);
    const double minutes = config_.number("files", "autosave.minutes", 10.0);
    if (!enabled || minutes <= 0.0) {
        autoSaveTimer_->stop();
        log(tr("Auto-save is off."));
        return;
    }

    autoSaveTimer_->start(static_cast<int>(minutes * 60.0 * 1000.0));
    log(tr("Auto-save every %1 minute(s) to %2")
            .arg(minutes)
            .arg(QString::fromStdWString(recovery_->directory().wstring())));
}

void MainWindow::autoSave() {
    if (document_ == nullptr || recovery_ == nullptr || !document_->isModified()) {
        return;
    }

    // Never mid-command. A command in progress may hold an open transaction,
    // and serialising a database in that state would capture a drawing halfway
    // through an edit — a recovery file that restores something the user never
    // saw. The next tick will catch it.
    if (document_->transactions().hasActiveTransaction() ||
        document_->executor().isRunning()) {
        return;
    }

    try {
        const std::string text = io::writeDxf(document_->db());
        if (recovery_->write(currentPath_.toStdString(), text)) {
            // Deliberately quiet on success. An auto-save that announces itself
            // every ten minutes trains the user to ignore the history panel,
            // which is where the failures need to be seen.
            return;
        }
        log(tr("! Auto-save failed to write to %1")
                .arg(QString::fromStdWString(recovery_->directory().wstring())));
    } catch (const std::exception& error) {
        // An auto-save that throws must not take the session with it: the whole
        // point is to be there when something else goes wrong.
        log(tr("! Auto-save failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void MainWindow::reportProblem(const QString& title, const QString& message,
                               const QString& remedy, const QString& detail) {
    // Always in the history, and in a dialog only when somebody is there to
    // dismiss it. A modal box in a scripted run does not report a failure --
    // it hangs, which is how a batch conversion or a capture harness stops
    // dead on the first unreadable file and says nothing about why.
    log(QStringLiteral("! %1: %2").arg(title, message));
    if (!remedy.isEmpty()) {
        log(QStringLiteral("  %1").arg(remedy));
    }
    if (nonInteractive_) {
        return;
    }

    QMessageBox box{this};
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(title);
    box.setText(message);
    if (!remedy.isEmpty()) {
        box.setInformativeText(remedy);
    }
    if (!detail.isEmpty()) {
        box.setDetailedText(detail);
    }
    box.exec();
}

void MainWindow::runScript(const QStringList& lines) {
    for (const QString& line : lines) {
        commandLine_->submitLine(line);
        // Let the canvas repaint and any queued work settle between lines, so a
        // script sees the same sequence of states a person would.
        QCoreApplication::processEvents();
    }
}

bool MainWindow::showRibbonTab(const QString& title) {
    return ribbon_ != nullptr && ribbon_->showTab(title);
}

void MainWindow::onProject() {
    // The folder the drawing is in, or the last folder used, is where a new
    // project goes: beside the drawings it describes.
    QString folder;
    if (document_ != nullptr && !document_->filePath.empty()) {
        folder = QFileInfo{QString::fromStdString(document_->filePath)}.absolutePath();
    }
    if (folder.isEmpty()) {
        folder = QDir::currentPath();
    }
    ProjectIndex& index = ensureProject();
    ProjectDialog dialog{index, folder, this};
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    log(tr("Project: %1 (%n sheet(s)) saved to %2", "",
           static_cast<int>(index.project().sheets.size()))
            .arg(QString::fromStdString(index.project().name),
                 QDir::toNativeSeparators(index.path())));
}

ProjectIndex& MainWindow::ensureProject() {
    if (project_ == nullptr) {
        project_ = std::make_unique<ProjectIndex>(translator_.get());
        if (agent_ != nullptr) {
            agent_->useProject(project_.get());
        }
    }
    return *project_;
}

void MainWindow::openProjectBeside(const QString& drawingPath) {
    // A project file beside the drawing is the set it belongs to. One
    // opened by hand stays: a drawing from elsewhere does not close it.
    if (project_ != nullptr && project_->isOpen() &&
        ProjectIndex::projectFileFor(drawingPath).isEmpty()) {
        return;
    }
    const bool had = project_ != nullptr && project_->isOpen();
    ensureProject().openBeside(drawingPath);
    if (project_->isOpen() && (!had || project_->path() != ProjectIndex::projectFileFor(drawingPath))) {
        log(tr("Project: %1 (%n sheet(s))", "", static_cast<int>(project_->project().sheets.size()))
                .arg(QString::fromStdString(project_->project().name)));
    }
}

void MainWindow::useProject(const QString& path) {
    QString error;
    if (!ensureProject().open(path, &error)) {
        log(tr("! %1").arg(error));
        return;
    }
    log(tr("Project: %1 (%n sheet(s))", "", static_cast<int>(project_->project().sheets.size()))
            .arg(QString::fromStdString(project_->project().name)));
}

AgentService& MainWindow::ensureAgent() {
    if (agent_ == nullptr) {
        agent_ = std::make_unique<AgentService>(config_, translator_.get(), settingsDirectory());
        if (project_ != nullptr) {
            agent_->useProject(project_.get());
        }
        // The assistant may ask the user a round of questions before it
        // answers. In a window they get a small form, each answer filled in
        // with the assistant's own suggestion so accepting is one click; a
        // scripted run has no one to ask and takes the suggestions.
        if (!nonInteractive_) {
            agent_->answerQuestionsWith(
                [this](const std::vector<AgentQuestion>& questions)
                    -> std::optional<std::vector<std::string>> {
                    QDialog dialog{this};
                    dialog.setWindowTitle(tr("The assistant asks"));
                    auto* layout = new QFormLayout{&dialog};
                    std::vector<QLineEdit*> edits;
                    for (const AgentQuestion& question : questions) {
                        auto* edit = new QLineEdit{QString::fromStdString(question.suggested),
                                                   &dialog};
                        edit->setMinimumWidth(320);
                        layout->addRow(QString::fromStdString(question.text), edit);
                        edits.push_back(edit);
                    }
                    auto* buttons = new QDialogButtonBox{
                        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog};
                    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                    layout->addRow(buttons);
                    if (dialog.exec() != QDialog::Accepted) {
                        return std::nullopt;
                    }
                    std::vector<std::string> answers;
                    for (const QLineEdit* edit : edits) {
                        answers.push_back(edit->text().trimmed().toStdString());
                    }
                    return answers;
                });
        }
    }
    return *agent_;
}

void MainWindow::onAgentSettings() {
    AgentSettingsDialog dialog{ensureAgent().settings(), this};
    dialog.setStandardsPreview(
        [this](const QString& directory) { return agent_->standardsPreview(directory); });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    agent_->setSettings(dialog.settings());
    config_.save(std::filesystem::path{configPath().toStdWString()});

    const AgentSettings& saved = agent_->settings();
    // Reported without the key, which is the one thing here that must not end
    // up in the history panel.
    log(saved.enabled ? tr("AI assistant: %1 at %2").arg(saved.model, saved.endpoint)
                      : tr("AI assistant is off."));
}

void MainWindow::useCannedAgentReply(const QString& path) {
    ensureAgent().useCannedReply(path);
}

void MainWindow::useAgentReference(const QString& directory) {
    ensureAgent().useReferenceDirectory(directory);
}

void MainWindow::showAgentContext() {
    if (document_ == nullptr) {
        return;
    }
    // What the model would be told, line by line, so a person can read it
    // without a key and without paying for the question.
    QStringList notes;
    const QString context = ensureAgent().context(*document_, &notes);
    for (const QString& line : notes) {
        log(line);
    }
    for (const QString& line : context.split(QLatin1Char('\n'))) {
        log(line);
    }
}

void MainWindow::runAgent(const QString& instruction) {
    if (document_ == nullptr) {
        return;
    }
    ensureAgent();

    // The wait is honest about itself: a request goes to somebody else's
    // machine and takes as long as it takes. Without this the window simply
    // stops responding and the user cannot tell it from a hang.
    std::unique_ptr<QProgressDialog> waiting;
    if (!nonInteractive_ && !agent_->isCanned()) {
        waiting = std::make_unique<QProgressDialog>(
            tr("Asking the assistant..."), QString{}, 0, 0, this);
        waiting->setWindowTitle(tr("AI Assistant"));
        waiting->setWindowModality(Qt::ApplicationModal);
        waiting->setMinimumDuration(0);
        waiting->show();
        QCoreApplication::processEvents();
    }

    AgentOutcome outcome = agent_->propose(instruction, *document_);
    waiting.reset();

    for (const QString& line : outcome.log) {
        log(line);
    }
    outcome.log.clear();

    if (!outcome.ok()) {
        log(tr("! %1").arg(outcome.error));
        if (!nonInteractive_) {
            QMessageBox::warning(this, tr("AI Assistant"), outcome.error);
        }
        onCommandStateChanged();
        return;
    }

    // Shown before it runs, and never applied without an answer.
    //
    // The steps are commands, so a person can read them and know exactly what
    // will happen -- which is the whole reason an assistant here emits commands
    // rather than reaching into the database. Approving blind would make that
    // property worthless.
    QStringList steps;
    for (const AgentCommand& command : outcome.plan.commands) {
        steps << QString::fromStdString(command.source);
    }
    if (!nonInteractive_) {
        const QString detail =
            tr("The assistant proposes %n step(s):\n\n%1", "", steps.size())
                .arg(steps.join(QStringLiteral("\n")));
        const auto answer = QMessageBox::question(
            this, tr("AI Assistant"), detail, QMessageBox::Apply | QMessageBox::Cancel,
            QMessageBox::Apply);
        if (answer != QMessageBox::Apply) {
            log(tr("Assistant: cancelled."));
            onCommandStateChanged();
            return;
        }
    } else {
        for (const QString& step : steps) {
            log(tr("Assistant proposes: %1").arg(step));
        }
    }

    agent_->apply(outcome, *document_);
    for (const QString& line : outcome.log) {
        log(line);
    }
    canvas_->update();
    onCommandStateChanged();
    updateWindowTitle();
}

bool MainWindow::showPanel(const QString& name) {
    const std::array<std::pair<const char*, QDockWidget*>, 3> panels{{
        {"layers", layerDock_},
        {"properties", propertiesDock_},
        {"history", historyDock_},
    }};
    for (const auto& [key, dock] : panels) {
        if (dock == nullptr || name.compare(QLatin1String{key}, Qt::CaseInsensitive) != 0) {
            continue;
        }
        dock->show();
        dock->raise();
        QCoreApplication::processEvents();
        return true;
    }
    log(tr("No panel named %1.").arg(name));
    return false;
}

bool MainWindow::showLayout(const QString& name) {
    if (layoutTabs_ == nullptr) {
        return false;
    }
    for (int i = 0; i < layoutTabs_->count(); ++i) {
        if (layoutTabs_->tabText(i).compare(name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        layoutTabs_->setCurrentIndex(i);
        QCoreApplication::processEvents();
        return true;
    }
    // Named loudly rather than ignored: a script asking for a sheet that is not
    // there has gone wrong, and silently photographing model space instead
    // would look like the sheet was empty.
    QStringList available;
    for (int i = 0; i < layoutTabs_->count(); ++i) {
        available << layoutTabs_->tabText(i);
    }
    log(tr("No layout named %1. This drawing has: %2")
            .arg(name, available.join(QStringLiteral(", "))));
    return false;
}

void MainWindow::cycleSelection() {
    canvas_->cycleHovered();
    QCoreApplication::processEvents();
}

QPoint MainWindow::canvasToWindow(const QPoint& canvasPosition) {
    return canvas_->mapTo(this, canvasPosition);
}

QPoint MainWindow::worldToCanvas(double x, double y) const {
    const render::ScreenPoint at = canvas_->toScreen(Vec3{x, y, 0.0});
    return QPoint{static_cast<int>(std::lround(at.x)), static_cast<int>(std::lround(at.y))};
}

bool MainWindow::resizeCanvasTo(int width, int height) {
    // Pin the drawing area, not the window.
    //
    // A scripted click is aimed at geometry, and where geometry lands on screen
    // depends on the size of the canvas and nothing else. Sizing the *window*
    // left every scenario's coordinates hostage to the chrome around it: the
    // day three rows above the drawing became one, seventeen scenarios started
    // clicking somewhere other than where they had been aimed -- not because
    // anything they tested had changed, but because the canvas had grown
    // forty-four pixels taller and the view had zoomed to suit.
    //
    // Solved by asking for the canvas and letting the window be whatever size
    // that needs. Iterated because the chrome can change height in response --
    // a ribbon wraps differently in a narrower window -- so one correction is
    // not always enough.
    for (int pass = 0; pass < 4; ++pass) {
        QCoreApplication::processEvents();
        const QSize have = canvas_->size();
        if (have == QSize{width, height}) {
            // Reported, because it is the frame every coordinate in a scripted
            // run is measured in, and a run that silently drew in a different
            // one would be very hard to account for afterwards.
            const QPoint at = canvas_->mapTo(this, QPoint{0, 0});
            log(tr("Canvas %1x%2 at (%3, %4) in a %5x%6 window.")
                    .arg(width)
                    .arg(height)
                    .arg(at.x())
                    .arg(at.y())
                    .arg(size().width())
                    .arg(size().height()));
            return true;
        }
        resize(size() + QSize{width - have.width(), height - have.height()});
    }
    QCoreApplication::processEvents();
    const QSize settled = canvas_->size();
    if (settled != QSize{width, height}) {
        log(tr("Could not size the canvas to %1x%2; it settled at %3x%4.")
                .arg(width)
                .arg(height)
                .arg(settled.width())
                .arg(settled.height()));
        return false;
    }
    return true;
}

void MainWindow::hoverAt(const QPoint& canvasPosition) {
    const QPoint windowPosition = canvasToWindow(canvasPosition);
    const QPointF local = canvas_->mapFrom(this, windowPosition);
    QMouseEvent move{QEvent::MouseMove,   local,          canvas_->mapToGlobal(local),
                     Qt::NoButton,        Qt::NoButton,   Qt::NoModifier};
    QCoreApplication::sendEvent(canvas_, &move);
    QCoreApplication::processEvents();
}

QWidget* MainWindow::widgetAt(const QPoint& windowPosition) {
    // The deepest child under the point, which is what the pointer would hit.
    //
    // Clicks used to go to the canvas unconditionally, so a scripted run could
    // reach the drawing and nothing else — not a ribbon tab, not a layer, not a
    // button. That made the whole interface outside the canvas untestable
    // headlessly, which is a poor reason for it to stay untested.
    QWidget* target = childAt(windowPosition);
    return target == nullptr ? canvas_ : target;
}

void MainWindow::snapMenuAt(const QPoint& canvasPosition) {
    hoverAt(canvasPosition);
    const QPoint windowPosition = canvasToWindow(canvasPosition);
    QWidget* target = widgetAt(windowPosition);
    const QPointF local = target->mapFrom(this, windowPosition);
    QMouseEvent press{QEvent::MouseButtonPress, local,           target->mapToGlobal(local),
                      Qt::RightButton,          Qt::RightButton, Qt::ShiftModifier};
    QCoreApplication::sendEvent(target, &press);
    QCoreApplication::processEvents();
}

void MainWindow::rightHoldAt(const QPoint& canvasPosition) {
    hoverAt(canvasPosition);
    const QPoint windowPosition = canvasToWindow(canvasPosition);

    QWidget* target = widgetAt(windowPosition);
    const QPointF local = target->mapFrom(this, windowPosition);
    const QPointF global = target->mapToGlobal(local);
    QMouseEvent press{QEvent::MouseButtonPress, local,           global,
                      Qt::RightButton,          Qt::RightButton, Qt::NoModifier};
    QCoreApplication::sendEvent(target, &press);

    // Held past the threshold, by letting the timer that measures it actually
    // run. Sending press and release back to back would only ever test the
    // quick click, which is the other behaviour entirely.
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 400) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

void MainWindow::clickAt(const QPoint& canvasPosition) {
    clickAt(canvasPosition, Qt::NoModifier);
}

void MainWindow::clickAt(const QPoint& canvasPosition, Qt::KeyboardModifiers modifiers) {
    // A move first, because picking follows the rollover: what a click selects
    // is whatever the cursor was last shown to be over.
    hoverAt(canvasPosition);

    const QPoint windowPosition = canvasToWindow(canvasPosition);
    QWidget* target = widgetAt(windowPosition);
    const QPointF local = target->mapFrom(this, windowPosition);
    const QPointF global = target->mapToGlobal(local);
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event{type,        local,          global,
                          Qt::LeftButton, Qt::LeftButton, modifiers};
        QCoreApplication::sendEvent(target, &event);
    }
    QCoreApplication::processEvents();
}

void MainWindow::doubleClickAt(const QPoint& canvasPosition) {
    // A real double-click is a press, a release, then a DblClick and its
    // release — Qt delivers exactly that, and the canvas acts on the DblClick.
    // Synthesising only the DblClick would test a path the mouse never takes.
    hoverAt(canvasPosition);

    const QPointF local = QPointF{canvasPosition};
    const QPointF global = canvas_->mapToGlobal(local);
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                                    QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
        QMouseEvent event{type,        local,          global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
        QCoreApplication::sendEvent(canvas_, &event);
    }
    QCoreApplication::processEvents();
}

void MainWindow::typeText(const QString& text) {
    // To whatever holds the keyboard, which is the point: this is how the
    // in-place text editor is reached, and it has the focus precisely when a
    // user would be typing into it.
    QWidget* target = QApplication::focusWidget();
    if (target == nullptr) {
        return;
    }
    for (const QChar character : text) {
        for (const QEvent::Type type : {QEvent::KeyPress, QEvent::KeyRelease}) {
            QKeyEvent event{type, 0, Qt::NoModifier, QString{character}};
            QCoreApplication::sendEvent(target, &event);
        }
    }
    QCoreApplication::processEvents();
}

void MainWindow::pressKey(const QString& spec) {
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    QString name = spec.trimmed().toLower();
    while (true) {
        if (name.startsWith("ctrl+")) {
            modifiers |= Qt::ControlModifier;
            name.remove(0, 5);
        } else if (name.startsWith("shift+")) {
            modifiers |= Qt::ShiftModifier;
            name.remove(0, 6);
        } else if (name.startsWith("alt+")) {
            modifiers |= Qt::AltModifier;
            name.remove(0, 4);
        } else {
            break;
        }
    }

    int key = 0;
    QString text;
    if (name == "return" || name == "enter") {
        key = Qt::Key_Return;
        text = QStringLiteral("\r");
    } else if (name == "escape" || name == "esc") {
        key = Qt::Key_Escape;
    } else if (name == "tab") {
        key = Qt::Key_Tab;
        text = QStringLiteral("\t");
    } else if (name == "backspace") {
        key = Qt::Key_Backspace;
    } else if (name == "space") {
        key = Qt::Key_Space;
        text = QStringLiteral(" ");
    } else if (name.size() >= 2 && name.at(0) == 'f' && name.mid(1).toInt() >= 1 &&
               name.mid(1).toInt() <= 12) {
        // The drafting toggles live on the function keys -- F8 is ortho, F3 the
        // running snaps -- so a script that cannot press one cannot test them.
        key = Qt::Key_F1 + name.mid(1).toInt() - 1;
    } else if (name.size() == 1) {
        key = name.at(0).toUpper().unicode();
        text = name;
    } else {
        return;
    }

    QWidget* target = QApplication::focusWidget();
    if (target == nullptr) {
        return;
    }
    for (const QEvent::Type type : {QEvent::KeyPress, QEvent::KeyRelease}) {
        QKeyEvent event{type, key, modifiers, text};
        QCoreApplication::sendEvent(target, &event);
    }
    QCoreApplication::processEvents();
}

bool MainWindow::grabTo(const QString& path) {
    // QWidget::grab renders through Qt rather than reading the screen, so it
    // works with no display at all — which is the point. Screen-scraping needs
    // the window to be visible and frontmost, and stealing focus from whoever
    // is using the machine is exactly what this avoids.
    return grab().save(path);
}

void MainWindow::offerRecovery() {
    if (recovery_ == nullptr || nonInteractive_) {
        return;
    }

    const std::vector<io::RecoveryEntry> pending =
        recovery_->pending([](std::uint64_t pid) { return isProcessRunning(pid); });
    if (pending.empty()) {
        return;
    }

    QStringList names;
    for (const io::RecoveryEntry& entry : pending) {
        names << tr("%1 — auto-saved %2")
                     .arg(QString::fromStdString(entry.displayName()),
                          QString::fromStdString(entry.savedAt));
    }

    // Two buttons, neither of which destroys anything.
    //
    // An earlier version offered "Discard" here and it was activated without
    // anyone pressing it — a stray activation on a dialog that appears at
    // startup deleted the very work it existed to protect. In a feature whose
    // whole job is not losing data, the destructive option must not be one
    // accident away from the default. Declining leaves the files on disk, which
    // is harmless and self-correcting: they are cleared on the next clean exit
    // once the work is saved, or deliberately from the Tools menu.
    QMessageBox box{this};
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Recover Unsaved Work"));
    box.setText(tr("%1 closed unexpectedly with unsaved changes.").arg(kDisplayName));
    box.setInformativeText(
        tr("%n drawing(s) can be recovered:", "", static_cast<int>(pending.size())) + "\n\n" +
        names.join("\n"));
    QPushButton* recover = box.addButton(tr("Recover"), QMessageBox::AcceptRole);
    box.addButton(tr("Not Now"), QMessageBox::RejectRole);
    box.setDefaultButton(recover);
    box.exec();

    if (box.clickedButton() == recover) {
        // Opened as an untitled drawing rather than under the original name, so
        // that saving is a deliberate act. Recovering must not overwrite the
        // file on disk before the user has looked at what came back.
        const io::RecoveryEntry& entry = pending.front();
        openFile(QString::fromStdWString(entry.recoveryFile.wstring()));
        currentPath_.clear();
        updateWindowTitle();
        log(tr("Recovered work auto-saved at %1. Save it somewhere to keep it.")
                .arg(QString::fromStdString(entry.savedAt)));
        if (pending.size() > 1) {
            log(tr("%1 further recovery file(s) are in %2")
                    .arg(pending.size() - 1)
                    .arg(QString::fromStdWString(recovery_->directory().wstring())));
        }
        return;
    }

    log(tr("%1 recovery file(s) kept in %2")
            .arg(pending.size())
            .arg(QString::fromStdWString(recovery_->directory().wstring())));
}

void MainWindow::onDiscardRecoveryFiles() {
    if (recovery_ == nullptr) {
        return;
    }
    const std::vector<io::RecoveryEntry> pending =
        recovery_->pending([](std::uint64_t pid) { return isProcessRunning(pid); });
    if (pending.empty()) {
        QMessageBox::information(this, tr("Recovery Files"),
                                 tr("There is no recovered work waiting."));
        return;
    }

    // Deliberate, confirmed, and never the default.
    QMessageBox box{this};
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Discard Recovered Work"));
    box.setText(tr("Permanently delete %n recovery file(s)?", "",
                   static_cast<int>(pending.size())));
    box.setInformativeText(tr("This cannot be undone."));
    QPushButton* keep = box.addButton(tr("Keep"), QMessageBox::RejectRole);
    QPushButton* del = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(keep);
    box.exec();

    if (box.clickedButton() == del) {
        for (const io::RecoveryEntry& entry : pending) {
            io::RecoveryStore::remove(entry);
        }
        log(tr("Discarded %1 recovery file(s).").arg(pending.size()));
    }
}

void MainWindow::writeBackup(const QString& path) const {
    if (!config_.boolean("files", "backup.enabled", true) || !QFileInfo::exists(path)) {
        return;
    }
    // The previous contents, kept under `.bak`, as other CAD software does. Cheap
    // insurance against a save that turns out to have been a mistake.
    const QString backup = path + ".bak";
    QFile::remove(backup);
    QFile::copy(path, backup);
}

void MainWindow::rememberRecentFile(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo{path}.absoluteFilePath();

    QStringList recent;
    for (const auto& [key, value] : config_.entries("recent")) {
        recent << QString::fromStdString(value);
    }
    recent.removeAll(absolute);
    recent.prepend(absolute);
    while (recent.size() > 10) {
        recent.removeLast();
    }

    for (int i = 0; i < 10; ++i) {
        const std::string key = "file" + std::to_string(i + 1);
        if (i < recent.size()) {
            config_.set("recent", key, recent[i].toStdString());
        } else {
            config_.remove("recent", key);
        }
    }
    config_.save(std::filesystem::path{configPath().toStdWString()});
    rebuildRecentFilesMenu();
}

void MainWindow::rebuildRecentFilesMenu() {
    if (recentFilesMenu_ == nullptr) {
        return;
    }
    recentFilesMenu_->clear();

    bool any = false;
    for (const auto& [key, value] : config_.entries("recent")) {
        const QString path = QString::fromStdString(value);
        if (path.isEmpty()) {
            continue;
        }
        any = true;
        // Shown by name with the full path as a tooltip: a menu of identical
        // filenames from different projects is no help.
        QAction* action = recentFilesMenu_->addAction(QFileInfo{path}.fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] {
            if (confirmDiscardChanges()) {
                openFile(path);
            }
        });
    }
    recentFilesMenu_->setEnabled(any);
}

void MainWindow::reportPlugins() {
    if (document_ == nullptr) {
        return;
    }
    for (const abi::PluginLoadResult& result : document_->pluginResults()) {
        const QString name = QString::fromStdWString(result.path.filename().wstring());
        const QString message = QString::fromStdString(result.message);
        log(result.loaded ? tr("Plugin %1 loaded: %2").arg(name, message)
                          : tr("! Plugin %1 not loaded: %2").arg(name, message));
    }
}

void MainWindow::onOpen() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Drawing"), {},
        tr("Drawings (*.dxf *.dwg);;DXF files (*.dxf);;DWG files (*.dwg);;All files (*)"));
    if (!path.isEmpty() && confirmDiscardChanges()) {
        openFile(path);
    }
}

void MainWindow::openFile(const QString& path) {
    const std::filesystem::path filePath = path.toStdWString();

    io::XrefLoadOptions options;
    // A drawing's own folder is always searched; naming it explicitly also
    // covers references stored as bare filenames.
    options.searchPaths.push_back(filePath.parent_path());
    // Supplied for every drawing, not only DWG ones: a DXF host may well
    // reference DWG xrefs, and there is no way to know before reading it.
    options.translator = translator_.get();
    options.onProgress = [this](const std::string& message) {
        log(QString::fromStdString(message));
        // The conversion blocks, so without this the message would not appear
        // until after the wait it is announcing.
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    };

    // Conversion of a large DWG takes seconds; the wait cursor is what stops
    // the user thinking the application has stopped responding.
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const auto restoreCursor = qScopeGuard([] { QGuiApplication::restoreOverrideCursor(); });

    try {
        if (io::isDwgPath(filePath)) {
            reportTranslatorIfUnavailable();
        }
        io::DrawingLoadResult loaded = io::loadDrawingWithReferences(filePath, options);
        // PDF pages have a size only their file knows; read it now so the
        // underlays have an outline before the drawing is first drawn.
        if (loaded.database != nullptr) {
            fillUnderlayPageSizes(*loaded.database);
        }

        std::unique_ptr<Document> opened = makeDocument();
        opened->reset(std::move(loaded.database));
        opened->filePath = filePath.string();
        openProjectBeside(path);

        // A second file is a second tab, not a replacement. The history and the
        // view belong to the drawing that produced them, so they are cleared
        // here for the incoming one rather than carried over from whichever
        // drawing happened to be open.
        document_ = nullptr;
        currentPath_ = path;
        loggedEntries_ = 0;
        commandLog_->clear();
        addDrawing(std::move(opened), path);

        // Recorded here, not only on save. Opening is how a drawing is most
        // often reached, and a recent list that fills only when you save a file
        // is empty for exactly the drawings you keep coming back to — the ones
        // you open, look at, and close.
        rememberRecentFile(path);

        log(tr("Opened %1").arg(QFileInfo{path}.fileName()));
        log(tr("  %1 objects · %2 layers")
                .arg(document_->db().objectCount())
                .arg(document_->db().layers().size()));
        reportPlugins();
        reportConfig();
        applyShortcuts();

        if (!loaded.xrefs.nodes.empty()) {
            log(tr("  %1 external reference(s): %2 resolved, %3 unresolved")
                    .arg(loaded.xrefs.nodes.size())
                    .arg(loaded.xrefs.resolvedCount)
                    .arg(loaded.xrefs.failedCount));
            for (const io::XrefNode& node : loaded.xrefs.nodes) {
                if (node.status != XrefStatus::Resolved) {
                    log(tr("  ! %1: %2")
                            .arg(QString::fromStdString(node.name),
                                 QString::fromStdString(node.problem)));
                }
            }
        }

        if (!loaded.attachments.empty()) {
            std::size_t missing = 0;
            for (const io::AttachedFileNode& node : loaded.attachments) {
                if (!node.found) {
                    ++missing;
                }
            }
            log(tr("  %1 attached file(s): %2 found, %3 missing")
                    .arg(loaded.attachments.size())
                    .arg(loaded.attachments.size() - missing)
                    .arg(missing));
            // Named individually, because a path is the only thing the user can
            // act on. "One image is missing" tells them there is a problem;
            // the path tells them where the file went.
            for (const io::AttachedFileNode& node : loaded.attachments) {
                if (node.found) {
                    continue;
                }
                log(tr("  ! %1 not found: %2")
                        .arg(QString::fromStdString(node.kind),
                             QString::fromStdString(node.storedPath)));
            }
        }

        // Warnings are expected on real files and are not failures; they are
        // surfaced so the user can see what was not understood.
        int warnings = 0;
        for (const io::DxfDiagnostic& diagnostic : loaded.diagnostics) {
            if (diagnostic.severity != io::DxfDiagnostic::Severity::Warning) {
                continue;
            }
            if (warnings < 20) {
                log(tr("  warning: %1").arg(QString::fromStdString(diagnostic.message)));
            }
            ++warnings;
        }
        if (warnings > 20) {
            log(tr("  … and %1 more warning(s)").arg(warnings - 20));
        }
        onCommandStateChanged();
        focusCommandLine();
    } catch (const io::TranslationError& error) {
        // A translation failure has a remedy attached, and showing it is the
        // difference between "cannot open" and "here is how to open it".
        reportProblem(tr("Cannot open drawing"), QString::fromUtf8(error.what()),
                      QString::fromStdString(error.remedy()), translatorSearchReport());
        log(tr("Failed to open %1: %2").arg(path, QString::fromUtf8(error.what())));
    } catch (const std::exception& error) {
        reportProblem(tr("Cannot open drawing"),
                      tr("%1\n\n%2").arg(path, QString::fromUtf8(error.what())));
        log(tr("Failed to open %1: %2").arg(path, QString::fromUtf8(error.what())));
    }
}

void MainWindow::reportTranslatorIfUnavailable() {
    if (translator_ == nullptr) {
        return;
    }
    const io::DwgTranslator::Status status = translator_->probe();
    if (status.available) {
        log(tr("DWG translator: %1").arg(QString::fromStdString(translator_->displayName())));
    } else {
        log(tr("! %1").arg(QString::fromStdString(status.problem)));
    }
}

QString MainWindow::translatorSearchReport() const {
    // Every place that was looked at, because "not found" without a list is a
    // dead end for anyone who has the converter installed somewhere unusual.
    QStringList lines;
    lines << tr("Looked for the ODA File Converter at:");
    for (const std::filesystem::path& location : io::OdaFileConverter::searchLocations()) {
        std::error_code ec;
        const bool present = std::filesystem::is_regular_file(location, ec);
        lines << QStringLiteral("  %1 %2")
                     .arg(present ? QStringLiteral("[found]") : QStringLiteral("[missing]"),
                          QString::fromStdWString(location.wstring()));
    }
    lines << QString{};
    lines << tr("Set %1 to override this search.")
                 .arg(QString::fromUtf8(io::OdaFileConverter::kPathEnvironmentVariable));
    return lines.join('\n');
}

bool MainWindow::saveTo(const QString& path) {
    if (document_ == nullptr) {
        return false;
    }

    try {
        io::DxfWriter writer;
        const std::filesystem::path target = path.toStdWString();

        // Before anything is written, so a save that turns out to have been a
        // mistake is still recoverable from `.bak`.
        writeBackup(path);

        // Written to a sibling temp file and then moved into place, so an
        // interrupted or failing write cannot destroy the user's existing
        // drawing — the file on disk is either the old one or the new one.
        const std::filesystem::path temporary = [&] {
            std::filesystem::path candidate = target;
            candidate += ".saving";
            return candidate;
        }();

        const std::string text = writer.write(document_->db());

        // DWG is written by handing the DXF to the translator, which is the
        // same tool that read the drawing in — this application still has no
        // DWG parser of its own and is not about to grow a guessed one.
        if (path.endsWith(".dwg", Qt::CaseInsensitive)) {
            if (translator_ == nullptr || !translator_->canWriteDwg()) {
                throw std::runtime_error(
                    "No DWG translator is available to write this file. Save as DXF instead.");
            }
            translator_->writeDwg(text, temporary);
            std::filesystem::rename(temporary, target);
        } else {
            std::ofstream stream{temporary, std::ios::binary};
            if (!stream) {
                throw std::runtime_error("Cannot create " + temporary.string());
            }
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!stream) {
                throw std::runtime_error("Failed while writing " + temporary.string());
            }
            stream.close();
            std::filesystem::rename(temporary, target);
        }

        for (const io::DxfDiagnostic& diagnostic : writer.diagnostics()) {
            log(tr("  warning: %1").arg(QString::fromStdString(diagnostic.message)));
        }

        const QString previousPath = currentPath_;
        document_->filePath = target.string();
        document_->markSaved();
        currentPath_ = path;
        openProjectBeside(path);

        // The work is on disk under its own name now, so the recovery copy has
        // nothing left to protect. Both names are cleared: Save As moves the
        // drawing, and the auto-save under the old name would otherwise be
        // offered back after a later crash as though it were unsaved work.
        if (recovery_ != nullptr) {
            recovery_->discard(previousPath.toStdString());
            recovery_->discard(currentPath_.toStdString());
        }
        rememberRecentFile(path);

        log(tr("Saved %1 (%2 bytes)")
                .arg(QFileInfo{path}.fileName())
                .arg(text.size()));
        updateWindowTitle();
        return true;
    } catch (const std::exception& error) {
        reportProblem(tr("Cannot save drawing"),
                      tr("%1\n\n%2").arg(path, QString::fromUtf8(error.what())));
        log(tr("Failed to save %1: %2").arg(path, QString::fromUtf8(error.what())));
        return false;
    }
}

bool MainWindow::saveDrawingTo(const QString& path) {
    return saveTo(path);
}

bool MainWindow::grabDialogTo(const QString& name, const QString& path) {
    if (document_ == nullptr) {
        return false;
    }
    std::unique_ptr<QDialog> dialog;
    if (name == "purge") {
        dialog = std::make_unique<PurgeDialog>(*document_, this);
    } else if (name == "references") {
        dialog = std::make_unique<ReferenceManagerDialog>(*document_, this);
    } else if (name == "plotsetup") {
        dialog = std::make_unique<PlotSetupDialog>(
            PlotSetup::load(config_), document_ != nullptr && document_->inPaperSpace(), this);
    } else if (name == "agent") {
        dialog = std::make_unique<AgentSettingsDialog>(ensureAgent().settings(), this);
    } else {
        log(tr("! No dialog named \"%1\".").arg(name));
        return false;
    }

    dialog->show();
    // Laid out before it is photographed, or the picture is of a window that
    // has not decided where anything goes yet.
    QApplication::processEvents();
    const bool grabbed = dialog->grab().save(path);
    dialog->close();
    return grabbed;
}

bool MainWindow::canSaveDwg() const {
    return translator_ != nullptr && translator_->canWriteDwg() && translator_->probe().available;
}

void MainWindow::onSave() {
    const bool dwg = currentPath_.endsWith(".dwg", Qt::CaseInsensitive);
    if (currentPath_.isEmpty() || (!currentPath_.endsWith(".dxf", Qt::CaseInsensitive) &&
                                   !(dwg && canSaveDwg()))) {
        // Save As rather than silently writing DXF over a path the drawing did
        // not come from — including a DWG this build has no translator to write.
        onSaveAs();
        return;
    }
    saveTo(currentPath_);
}

void MainWindow::onSaveAs() {
    // Offered in the format the drawing arrived in. A drawing opened from DWG
    // whose owner expects a DWG back should not have to notice that this
    // application's native form is DXF, and defaulting to DXF is how a set ends
    // up half in one format and half in the other.
    const bool dwg = canSaveDwg();
    const bool cameFromDwg = currentPath_.endsWith(".dwg", Qt::CaseInsensitive);
    const QString preferred = dwg && cameFromDwg ? QStringLiteral(".dwg") : QStringLiteral(".dxf");

    const QString suggested =
        currentPath_.isEmpty() ? QStringLiteral("Untitled") + preferred
                               : QFileInfo{currentPath_}.completeBaseName() + preferred;

    // Only what can actually be produced is listed. A DWG entry that fails when
    // chosen is worse than no entry at all, because it is discovered after the
    // user has committed to a name.
    QStringList filters;
    if (dwg && cameFromDwg) {
        filters << tr("DWG files (*.dwg)") << tr("DXF files (*.dxf)");
    } else if (dwg) {
        filters << tr("DXF files (*.dxf)") << tr("DWG files (*.dwg)");
    } else {
        filters << tr("DXF files (*.dxf)");
    }

    QString selected = filters.front();
    QString path = QFileDialog::getSaveFileName(this, tr("Save Drawing"), suggested,
                                                filters.join(QStringLiteral(";;")), &selected);
    if (path.isEmpty()) {
        return;
    }
    // A name typed without an extension takes the one the chosen filter names,
    // rather than landing on disk with no extension at all.
    if (!path.endsWith(".dxf", Qt::CaseInsensitive) &&
        !path.endsWith(".dwg", Qt::CaseInsensitive)) {
        path += selected.contains(QStringLiteral("*.dwg")) ? QStringLiteral(".dwg")
                                                           : QStringLiteral(".dxf");
    }
    saveTo(path);
}

bool MainWindow::confirmDiscardChanges() {
    if (document_ == nullptr || !document_->isModified()) {
        return true;
    }
    if (nonInteractive_) {
        // Nobody is watching to answer. Refusing is the safe answer: a script
        // that would have discarded somebody's unsaved work silently is worse
        // than one that stops and says why.
        log(tr("! Unsaved changes; not continuing without an answer."));
        return false;
    }

    const QString name =
        currentPath_.isEmpty() ? tr("Untitled") : QFileInfo{currentPath_}.fileName();
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("%1 has unsaved changes.\n\nSave before continuing?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    switch (answer) {
        case QMessageBox::Save:
            onSave();
            // If the save was cancelled or failed the document is still dirty,
            // so the caller must not proceed to discard it.
            return document_ == nullptr || !document_->isModified();
        case QMessageBox::Discard:
            return true;
        default:
            return false;
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!confirmDiscardChanges()) {
        event->ignore();
        return;
    }
    // A clean exit means there is nothing to recover. Leaving the files behind
    // would greet the user on next launch with an offer to restore work they
    // deliberately saved or discarded — which teaches them to dismiss the
    // prompt without reading it, exactly when it will one day matter.
    if (recovery_ != nullptr) {
        recovery_->discardAll();
    }
    event->accept();
}

void MainWindow::onUndo() {
    if (document_ == nullptr) {
        return;
    }
    document_->executor().execute("UNDO");
    document_->clearSelection();
    document_->reindex();
    onSelectionChanged();
    onCommandStateChanged();
}

void MainWindow::onRedo() {
    if (document_ == nullptr) {
        return;
    }
    document_->executor().execute("REDO");
    document_->clearSelection();
    document_->reindex();
    onSelectionChanged();
    onCommandStateChanged();
}

void MainWindow::onZoomExtents() {
    canvas_->zoomExtents();
}

void MainWindow::refreshLayerPanel() {
    // Both surfaces onto the same layer table: the ribbon strip for the
    // current layer and its three switches, the dock for everything else.
    // Refreshed together so they cannot disagree about what is current.
    layers_->refresh();
    if (layerRibbon_ != nullptr) {
        layerRibbon_->refresh();
    }
}

void MainWindow::setPanelDocument() {
    properties_->setDocument(document_);
    layers_->setDocument(document_);
    if (layerRibbon_ != nullptr) {
        layerRibbon_->setDocument(document_);
    }
}

void MainWindow::refreshDynamicInput() {
    if (document_ == nullptr) {
        canvas_->hideDynamicInput();
        return;
    }
    // Never while the in-place editor is open. The two say the same thing —
    // one is the prompt for the text being edited, the other *is* the text
    // being edited — and the prompt lands on top of the editor, so the note
    // appears twice with the copy nobody is typing into in front.
    if (canvas_->isEditingText()) {
        canvas_->hideDynamicInput();
        return;
    }
    // The dimensional input boxes carry the prompt themselves, so the floating
    // one is suppressed while they are up. Both anchor to the crosshair, and
    // with both showing the prompt lands across the numbers being typed into.
    //
    // Checked before the grip case, because a held grip shows the boxes too and
    // they still need to be told what is being typed.
    if (canvas_->isEnteringDimension()) {
        canvas_->setDimensionTyped(commandLine_->currentText());
        canvas_->hideDynamicInput();
        return;
    }
    // A grip held but not taking dimensions -- naming a base point -- keeps its
    // own readout. No command is running, so this would otherwise replace the
    // measurement with "Command:" on the first mouse move.
    if (canvas_->isEditingGrip()) {
        return;
    }

    const QString typed = commandLine_->currentText();
    const bool running = document_->executor().isRunning();

    // Shown while something is happening: a command is prompting, or the user
    // has started typing. Not otherwise — a permanent box under the crosshair
    // would be in the way rather than helpful.
    if (typed.isEmpty() && !running) {
        canvas_->hideDynamicInput();
        return;
    }
    canvas_->showDynamicInput(running ? commandLine_->promptText() : tr("Command:"), typed);
}

void MainWindow::refreshLayoutTabs() {
    if (document_ == nullptr) {
        return;
    }
    // Rebuilding emits currentChanged for each insertion, which would switch
    // the document's layout as a side effect of describing it.
    const QSignalBlocker blocker{layoutTabs_};
    while (layoutTabs_->count() > 0) {
        layoutTabs_->removeTab(0);
    }

    const std::string active = document_->activeLayout().name;
    int activeIndex = 0;
    for (const Layout* layout : document_->db().layouts()) {
        const int index = layoutTabs_->addTab(QString::fromStdString(layout->name));
        layoutTabs_->setTabData(index, QString::fromStdString(layout->name));
        if (!layout->isModel()) {
            // Shown in the sheet's own units, which is what is drawn and what
            // the drafter measures against — not the millimetres DXF stores.
            const Vec3 size = layout->plot.sheetSize();
            const QString unit = layout->plot.paperUnits == PlotPaperUnits::Inches ? tr("in")
                                 : layout->plot.paperUnits == PlotPaperUnits::Pixels
                                     ? tr("px")
                                     : tr("mm");
            layoutTabs_->setTabToolTip(index, tr("%1 × %2 %3")
                                                  .arg(size.x, 0, 'f', 1)
                                                  .arg(size.y, 0, 'f', 1)
                                                  .arg(unit));
        }
        if (layout->name == active) {
            activeIndex = index;
        }
    }
    // The block being edited, as a tab of its own after the sheets. It carries
    // no layout name in its data, which is how `onLayoutTabChanged` tells it
    // apart from a sheet without having to be told twice.
    if (const BlockTableRecord* block = document_->editedBlock()) {
        const int index =
            layoutTabs_->addTab(tr("Block: %1").arg(QString::fromStdString(block->name)));
        layoutTabs_->setTabToolTip(
            index, tr("Editing the definition of %1. Every place it is inserted shows the "
                      "change. Choose another tab to close it.")
                       .arg(QString::fromStdString(block->name)));
        activeIndex = index;
    }

    layoutTabs_->setCurrentIndex(activeIndex);
    // A drawing with only a Model tab has nothing to switch between, and a
    // single dead tab is chrome rather than information.
    layoutTabs_->setVisible(layoutTabs_->count() > 1);
}

void MainWindow::onLayoutTabChanged(int index) {
    if (document_ == nullptr || index < 0) {
        return;
    }
    const QString name = layoutTabs_->tabData(index).toString();
    if (name.isEmpty()) {
        // The block tab, which is already the thing being shown.
        return;
    }
    // Choosing a sheet is how the block editor is closed. There is nothing to
    // save on the way out: the block was edited where it lives.
    const bool wasEditingBlock = document_->editedBlock() != nullptr;
    document_->endEditingBlock();
    if (!document_->setActiveLayout(name.toStdString())) {
        return;
    }
    if (wasEditingBlock) {
        document_->reindex();
        log(tr("Closed the block editor."));
        refreshLayoutTabs();
    }

    // Switching space invalidates the selection — the entities selected in the
    // model are not on this sheet — and the view, which was framed for a
    // different coordinate system entirely.
    document_->clearSelection();
    canvas_->zoomExtents();
    onSelectionChanged();
    updateWindowTitle();
    log(tr("Switched to %1").arg(name));
}

void MainWindow::refreshCommandLog() {
    if (document_ == nullptr) {
        return;
    }
    const auto& history = document_->executor().history();

    // The executor's history is capped and drops from the front, so a shrink
    // means entries were discarded and the mirror must resynchronize.
    if (history.size() < loggedEntries_) {
        loggedEntries_ = 0;
        commandLog_->clear();
    }

    for (std::size_t i = loggedEntries_; i < history.size(); ++i) {
        const HistoryEntry& entry = history[i];
        QString text = QString::fromStdString(entry.text);
        switch (entry.kind) {
            case HistoryEntry::Kind::Command: text = "> " + text; break;
            case HistoryEntry::Kind::Error:   text = "✗ " + text; break;
            case HistoryEntry::Kind::Warning: text = "! " + text; break;
            default: break;
        }
        if (!text.trimmed().isEmpty()) {
            commandLog_->appendPlainText(text);
            // To the console too when nobody is watching the window, for the
            // same reason `log` does it: a scripted run that only reports an
            // exit code cannot say what a command actually replied.
            if (nonInteractive_) {
                std::fputs(text.toUtf8().constData(), stderr);
                std::fputc('\n', stderr);
            }
        }
    }
    loggedEntries_ = history.size();
}

void MainWindow::onCursorMoved(double worldX, double worldY) {
    coordinateLabel_->setText(
        QString{"%1, %2"}.arg(formatCoordinate(worldX), formatCoordinate(worldY)));
}

void MainWindow::onViewChanged() {
    zoomLabel_->setText(QString{"%1%"}.arg(canvas_->viewport().scale() * 100.0, 0, 'f', 1));
    statsLabel_->setText(tr("%1 visible · build %2 ms · paint %3 ms")
                             .arg(canvas_->lastVisibleCount())
                             .arg(canvas_->lastBuildMilliseconds(), 0, 'f', 1)
                             .arg(canvas_->lastPaintMilliseconds(), 0, 'f', 1));
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    // Escape means "stop what I am doing", and what that is depends on what is
    // going on — in this order, because each is nearer to the user's attention
    // than the one after it:
    //
    //   1. a running command, which is cancelled;
    //   2. a selection, which is cleared;
    //   3. nothing, and the key falls through.
    //
    // The command line already does this, and only while it holds the keyboard.
    // Clicking a ribbon button or a panel moves focus away, and Escape then did
    // nothing at all — which is exactly when a user reaches for it, having just
    // clicked something they did not mean to.
    if (event->key() != Qt::Key_Escape || !escapePressed()) {
        QMainWindow::keyPressEvent(event);
        return;
    }
    event->accept();
}

bool MainWindow::escapePressed() {
    if (document_ == nullptr) {
        return false;
    }
    if (document_->executor().isRunning()) {
        document_->executor().supply(InputValue::makeCancel());
        document_->reindex();
        onCommandStateChanged();
        return true;
    }
    // Before the selection, because a grip edit is the narrower thing to be in
    // the middle of and Escape should undo the smallest step available.
    if (canvas_ != nullptr && canvas_->cancelGripEdit()) {
        onCommandStateChanged();
        return true;
    }
    if (!document_->selection().empty()) {
        document_->clearSelection();
        onCommandStateChanged();
        return true;
    }
    return false;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress) {
        return QMainWindow::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    // F8 toggles ortho wherever the focus is, which is how it is pressed:
    // mid-drag, without looking, while the pointer is over the drawing.
    if (key->key() == Qt::Key_F8) {
        toggleOrtho();
        return true;
    }
    if (key->key() == Qt::Key_F10) {
        togglePolar();
        return true;
    }
    if (key->key() == Qt::Key_F3) {
        toggleObjectSnap();
        return true;
    }
    if (key->key() == Qt::Key_F11) {
        toggleTracking();
        return true;
    }
    // Tab moves between the dimension boxes, locking the one it leaves --
    // other CAD software's gesture for "this length, now let me set the angle". Only while
    // they are up; everywhere else Tab still moves the focus.
    if (key->key() == Qt::Key_Tab && canvas_->isEnteringDimension()) {
        if (canvas_->advanceDimensionField(commandLine_->currentText())) {
            commandLine_->clearInput();
            return true;
        }
    }
    // Space steps the grip mode, as it does in other CAD software -- but only with an
    // empty command line. A drafter part-way through typing a distance means
    // the space to be a space, or nothing typed after it would arrive.
    if ((key->key() == Qt::Key_Space || key->key() == Qt::Key_Return ||
         key->key() == Qt::Key_Enter) &&
        canvas_ != nullptr && canvas_->isEditingGrip() && commandLine_ != nullptr &&
        commandLine_->currentText().isEmpty()) {
        canvas_->cycleGripMode();
        return true;
    }
    if (key->key() != Qt::Key_Escape) {
        return QMainWindow::eventFilter(watched, event);
    }

    // A dialog's own Escape closes the dialog, and the in-place editor's
    // abandons the edit. Both are nearer the user's attention than a command
    // running behind them, so neither is taken away.
    if (QApplication::activeModalWidget() != nullptr) {
        return QMainWindow::eventFilter(watched, event);
    }
    if (canvas_ != nullptr && canvas_->isEditingText()) {
        return QMainWindow::eventFilter(watched, event);
    }
    // Only keys headed for this window. A second top-level window is not this
    // window's to interfere with.
    if (const auto* widget = qobject_cast<QWidget*>(watched);
        widget != nullptr && widget->window() != this) {
        return QMainWindow::eventFilter(watched, event);
    }

    if (escapePressed()) {
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onEditBlock(Handle blockRecord) {
    if (document_ == nullptr) {
        return;
    }
    const auto* block = document_->db().get<BlockTableRecord>(blockRecord);
    if (block == nullptr) {
        return;
    }
    if (block->isXref()) {
        // An xref's contents belong to another drawing, and editing them here
        // would be editing a copy that the next reload discards.
        log(tr("! %1 is a reference to another drawing; open that drawing to edit it.")
                .arg(QString::fromStdString(block->name)));
        return;
    }
    if (!document_->beginEditingBlock(blockRecord)) {
        return;
    }

    // The block is drawn in its own coordinates, which are nowhere near where
    // the insert was on screen.
    document_->clearSelection();
    // The index holds whichever space is being edited, so it has to be rebuilt
    // before anything tries to pick or frame the new one.
    document_->reindex();
    canvas_->reload();
    canvas_->zoomExtents();
    refreshLayoutTabs();
    onSelectionChanged();
    updateWindowTitle();
    log(tr("Editing block %1. Changes appear everywhere it is inserted.")
            .arg(QString::fromStdString(block->name)));
}

void MainWindow::toggleLineweights() {
    if (document_ == nullptr) {
        return;
    }
    bool& shown = document_->db().settings.displayLineweights;
    shown = !shown;
    if (lineweightButton_ != nullptr) {
        lineweightButton_->setChecked(shown);
    }
    log(shown ? tr("Lineweights shown at their width.") : tr("Lineweights shown as hairlines."));
    canvas_->update();
}

void MainWindow::toggleOrtho() {
    if (document_ == nullptr) {
        return;
    }
    const bool enabled = !canvas_->orthoEnabled();
    canvas_->setOrthoEnabled(enabled);
    if (enabled) {
        canvas_->setPolarEnabled(false);
        canvas_->setOrthoEnabled(true);
    }
    if (orthoButton_ != nullptr) {
        orthoButton_->setChecked(canvas_->orthoEnabled());
    }
    if (polarButton_ != nullptr) {
        polarButton_->setChecked(canvas_->polarEnabled());
    }
    // Reported in the history as other CAD software does, because F8 is pressed without
    // looking and the drafter needs to know which way it went.
    log(enabled ? tr("<Ortho on>") : tr("<Ortho off>"));
}

void MainWindow::togglePolar() {
    if (document_ == nullptr) {
        return;
    }
    const bool enabled = !canvas_->polarEnabled();
    canvas_->setPolarEnabled(enabled);
    if (polarButton_ != nullptr) {
        polarButton_->setChecked(enabled);
    }
    // Turning polar on turns ortho off, so the buttons must agree with the
    // drawing rather than with the last thing clicked.
    if (orthoButton_ != nullptr) {
        orthoButton_->setChecked(canvas_->orthoEnabled());
    }
    log(enabled ? tr("<Polar on>") : tr("<Polar off>"));
}

void MainWindow::toggleTracking() {
    if (document_ == nullptr) {
        return;
    }
    const bool enabled = !canvas_->trackingEnabled();
    canvas_->setTrackingEnabled(enabled);
    if (trackingButton_ != nullptr) {
        trackingButton_->setChecked(enabled);
    }
    log(enabled ? tr("<Otrack on>") : tr("<Otrack off>"));
}

void MainWindow::toggleObjectSnap() {
    if (document_ == nullptr || snapButton_ == nullptr) {
        return;
    }
    const bool enabled = canvas_->snapModes() == 0;
    snapButton_->setChecked(enabled);
    setSnapModes(enabled ? savedSnapModes_ : 0);
    log(enabled ? tr("<Osnap on>") : tr("<Osnap off>"));
}

void MainWindow::popupMenu(QMenu& menu, const QPoint& globalPosition) {
    menu.setAttribute(Qt::WA_DeleteOnClose);
    if (nonInteractive_) {
        // `exec` is modal and would stop a scripted run dead at the moment the
        // menu appeared. A popup menu is its own top-level window, which a grab
        // of *this* window does not contain -- so what it offers is written
        // down instead. A list of the actions is the part worth checking
        // anyway: the menu is right or wrong by what is in it.
        QStringList labels;
        for (const QAction* action : menu.actions()) {
            if (action->isSeparator()) {
                labels << QStringLiteral("|");
                continue;
            }
            // The tick too, since on a menu of modes it is the whole answer:
            // the list of names is the same whichever mode is current.
            labels << (action->isChecked() ? QStringLiteral("[x] ") : QString{}) + action->text();
        }
        log(tr("Menu: %1").arg(labels.join(QStringLiteral("  "))));
        menu.popup(globalPosition);
        scriptedMenu_ = &menu;
        QCoreApplication::processEvents();
        return;
    }
    menu.exec(globalPosition);
}

bool MainWindow::pickMenuAction(const QString& label) {
    if (scriptedMenu_.isNull()) {
        // Nothing popped up, so this is a menu bar item. Without this the whole
        // menu bar was unreachable from a scripted run: its menus are built
        // from the registry and never opened, so nothing checked that an item
        // on one does what it says.
        return pickMenuBarAction(label);
    }
    // Ampersands are mnemonics, not text the caller should have to guess at,
    // and case is not worth being strict about in a test script.
    const auto plain = [](QString text) {
        return text.remove(QLatin1Char('&')).trimmed().toLower();
    };
    const QString wanted = plain(label);
    for (QAction* action : scriptedMenu_->actions()) {
        if (action->isSeparator() || plain(action->text()) != wanted) {
            continue;
        }
        QMenu* menu = scriptedMenu_;
        scriptedMenu_ = nullptr;
        const QString text = action->text();
        // Hidden, not closed. The menu is `WA_DeleteOnClose`, and the action
        // about to be triggered is one of its children -- closing first
        // deletes the thing being triggered. Hiding gets the popup out of the
        // way while it still owns what is needed.
        menu->hide();
        action->trigger();
        QCoreApplication::processEvents();
        // Now nothing points into it. If the trigger opened another menu, that
        // one has already claimed `scriptedMenu_` and is a different object.
        menu->close();
        log(tr("Picked: %1").arg(text));
        return true;
    }
    log(tr("No menu item named %1.").arg(label));
    return false;
}

bool MainWindow::pickMenuBarAction(const QString& label) {
    const auto plain = [](QString text) {
        return text.remove(QLatin1Char{'&'}).trimmed().toLower();
    };
    const QString wanted = plain(label);

    QAction* found = nullptr;
    const std::function<void(QMenu*)> walk = [&](QMenu* menu) {
        for (QAction* action : menu->actions()) {
            if (found != nullptr) {
                return;
            }
            if (QMenu* child = action->menu(); child != nullptr) {
                walk(child);
                continue;
            }
            // By label, or by the command name the action carries -- a script
            // asking for LINE should not have to know it reads "Line" here.
            if (plain(action->text()) == wanted ||
                plain(action->data().toString()) == wanted) {
                found = action;
            }
        }
    };
    for (QAction* action : menuBar()->actions()) {
        if (QMenu* menu = action->menu(); menu != nullptr) {
            walk(menu);
        }
    }

    if (found == nullptr) {
        log(tr("No menu item named %1.").arg(label));
        return false;
    }
    found->trigger();
    QCoreApplication::processEvents();
    log(tr("Picked: %1").arg(found->text().remove(QLatin1Char{'&'})));
    return true;
}

QStringList MainWindow::droppedDrawings(const QMimeData* mime) {
    QStringList paths;
    if (mime == nullptr || !mime->hasUrls()) {
        return paths;
    }
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();
        if (path.endsWith(QStringLiteral(".dwg"), Qt::CaseInsensitive) ||
            path.endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive)) {
            paths << path;
        }
    }
    return paths;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    // Accepted only for drawings, so the desktop shows the refusal cursor for
    // anything else rather than a drop that then does nothing.
    if (droppedDrawings(event->mimeData()).isEmpty()) {
        QMainWindow::dragEnterEvent(event);
        return;
    }
    event->acceptProposedAction();
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event) {
    if (droppedDrawings(event->mimeData()).isEmpty()) {
        QMainWindow::dragMoveEvent(event);
        return;
    }
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QStringList paths = droppedDrawings(event->mimeData());
    if (paths.isEmpty()) {
        QMainWindow::dropEvent(event);
        return;
    }
    event->acceptProposedAction();
    // Each in a tab of its own, in the order they were dragged; the last one
    // ends up in front, which is what opening several files one after another
    // does too.
    for (const QString& path : paths) {
        openFile(path);
    }
}

bool MainWindow::dropFilesForScript(const QStringList& paths) {
    QMimeData mime;
    QList<QUrl> urls;
    for (const QString& path : paths) {
        urls << QUrl::fromLocalFile(QFileInfo{path}.absoluteFilePath());
    }
    mime.setUrls(urls);

    const QPoint where = rect().center();
    QDragEnterEvent enter{where, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(this, &enter);
    if (!enter.isAccepted()) {
        log(tr("Ignored a drop that was not a drawing: %1").arg(paths.join(QStringLiteral(", "))));
        return false;
    }
    QDropEvent drop{where, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier};
    QCoreApplication::sendEvent(this, &drop);
    QCoreApplication::processEvents();
    return true;
}

void MainWindow::showCanvasContextMenu(const QPoint& globalPosition) {
    if (document_ == nullptr) {
        return;
    }
    auto* owned = new QMenu{this};
    QMenu& menu = *owned;

    const auto command = [&](const QString& label, const char* name) {
        menu.addAction(label, this, [this, name] { runCommand(name); });
    };

    // A held grip is its own situation, and the one other CAD software puts a menu on: no
    // command is running, so without this the menu would offer "repeat the last
    // command" to somebody in the middle of moving something.
    if (canvas_->isEditingGrip()) {
        // Every mode by name with the current one ticked, which is how other CAD software
        // presents them. A drafter reaching for Rotate should find the word
        // "Rotate", not have to count Space presses to get there.
        const QString current = canvas_->currentGripMode();
        auto* modes = new QActionGroup{&menu};
        modes->setExclusive(true);
        for (const QString& mode : CanvasWidget::gripModes()) {
            // Title case: the menu is prose, and the prompt's shouting
            // capitals belong on the command line, not in a list of choices.
            QAction* action = menu.addAction(mode.left(1) + mode.mid(1).toLower(), this,
                                             [this, mode] { canvas_->setGripMode(mode); });
            action->setCheckable(true);
            action->setChecked(mode == current);
            modes->addAction(action);
        }
        menu.addSeparator();
        for (const auto& [label, option] :
             {std::pair{tr("Copy (multiple)"), "Copy"}, std::pair{tr("Base Point"), "Base"},
              std::pair{tr("Undo"), "Undo"}, std::pair{tr("Exit"), "Exit"}}) {
            menu.addAction(label, this,
                           [this, option] { canvas_->applyGripOption(QString::fromLatin1(option)); });
        }
        popupMenu(menu, globalPosition);
        return;
    }

    if (document_->executor().isRunning()) {
        // Mid-command the only useful answers are about the command.
        menu.addAction(tr("Enter"), this, [this] {
            document_->executor().supply(InputValue::makeEmpty());
            document_->reindex();
            onCommandStateChanged();
        });
        menu.addAction(tr("Cancel"), this, [this] { static_cast<void>(escapePressed()); });
        // Whatever the command itself offers, by the words it offers them in.
        if (const InputRequest* request = document_->executor().currentRequest();
            request != nullptr && !request->keywords.empty()) {
            menu.addSeparator();
            for (const CommandKeyword& keyword : request->keywords) {
                const QString word = QString::fromStdString(keyword.name);
                menu.addAction(word, this, [this, word] {
                    document_->executor().supply(InputValue::makeKeyword(word.toStdString()));
                    document_->reindex();
                    onCommandStateChanged();
                });
            }
        }
    } else if (!document_->selection().empty()) {
        // Something is selected: the clipboard first, because that is where the
        // hand goes and because these are the entries every other application
        // puts at the top of a right-click on a selection.
        command(tr("Cut"), "CUTCLIP");
        command(tr("Copy"), "COPYCLIP");
        menu.addSeparator();
        // Then what is done to things. "Copy Selection" rather than "Copy":
        // this one duplicates inside the drawing, and other CAD software gives it the
        // longer name for the same reason -- one menu cannot have two Copies.
        command(tr("Move"), "MOVE");
        command(tr("Copy Selection"), "COPY");
        command(tr("Rotate"), "ROTATE");
        command(tr("Scale"), "SCALE");
        command(tr("Mirror"), "MIRROR");
        menu.addSeparator();
        command(tr("Erase"), "ERASE");
        menu.addSeparator();
        menu.addAction(tr("Deselect All"), this, [this] {
            document_->clearSelection();
            onCommandStateChanged();
        });
    } else {
        menu.addAction(tr("Repeat Last Command"), this, [this] {
            document_->executor().repeatLast();
            onCommandStateChanged();
        });
        menu.addSeparator();
        command(tr("Paste"), "PASTECLIP");
        menu.addSeparator();
        command(tr("Undo"), "UNDO");
        command(tr("Redo"), "REDO");
        menu.addSeparator();
        command(tr("Zoom Extents"), "ZOOMEXTENTS");
        command(tr("Pan"), "PAN");
    }

    popupMenu(menu, globalPosition);
}

void MainWindow::showSnapOverrideMenu(const QPoint& globalPosition) {
    if (document_ == nullptr) {
        return;
    }
    auto* owned = new QMenu{this};
    QMenu& menu = *owned;
    menu.addAction(tr("None (this point only)"), this,
                   [this] { canvas_->setSnapOverride(SnapMode::None); });
    menu.addSeparator();
    for (const SnapMode mode : allSnapModes()) {
        if (mode == SnapMode::Clear || mode == SnapMode::None) {
            continue;
        }
        const QString name =
            QString::fromUtf8(snapModeName(mode).data(), static_cast<int>(snapModeName(mode).size()));
        menu.addAction(name, this, [this, mode] { canvas_->setSnapOverride(mode); });
    }
    popupMenu(menu, globalPosition);
}

void MainWindow::onPurge() {
    if (document_ == nullptr) {
        return;
    }
    if (nonInteractive_) {
        log(tr("! PURGE opens a dialog. Use -PURGE, which asks at the command line."));
        return;
    }
    PurgeDialog dialog{*document_, this};
    dialog.exec();
    // Whatever went, went from the drawing: the canvas is showing a database
    // that has changed underneath it.
    canvas_->reload();
    refreshLayerPanel();
}

AttachHooks MainWindow::attachHooks() {
    AttachHooks hooks;
    hooks.xref = [this](const std::string& path, bool overlay, Handle* block) {
        return attachExternalReference(path, overlay, block);
    };
    hooks.pdfPages = [](const std::string& path, int* pages) -> std::string {
        const QString file = QString::fromStdString(path);
        if (!QFileInfo::exists(file)) {
            return "There is no file at " + path + ".";
        }
        *pages = pdfPageCount(file);
        if (*pages <= 0) {
            return "The PDF could not be opened: " + path;
        }
        return {};
    };
    hooks.pdfPage = [](const std::string& path, int page, double* width,
                       double* height) -> std::string {
        const std::optional<QSizeF> size = pdfPageSizePoints(QString::fromStdString(path), page);
        if (!size) {
            return "The PDF has no page " + std::to_string(page) + ".";
        }
        *width = size->width();
        *height = size->height();
        return {};
    };
    hooks.imageSize = [](const std::string& path, int* width, int* height) -> std::string {
        const QString file = QString::fromStdString(path);
        if (!QFileInfo::exists(file)) {
            return "There is no file at " + path + ".";
        }
        const QSize size = QImageReader{file}.size();
        if (!size.isValid() || size.isEmpty()) {
            return "The image could not be read: " + path;
        }
        *width = size.width();
        *height = size.height();
        return {};
    };
    return hooks;
}

std::string MainWindow::attachExternalReference(const std::string& path, bool overlay,
                                                Handle* block) {
    if (document_ == nullptr) {
        return "No drawing is open.";
    }
    const std::filesystem::path file{QString::fromStdString(path).toStdWString()};
    if (!std::filesystem::exists(file)) {
        return "There is no file at " + path + ".";
    }
    Database& db = document_->db();
    const std::string name = file.stem().string();
    if (BlockTableRecord* existing = db.findBlock(name); existing != nullptr) {
        if (!existing->isXref()) {
            return "A block named \"" + name + "\" already exists and is not a reference.";
        }
        // Already attached: this is another reference to the same file.
        *block = existing->handle;
        return {};
    }

    auto record = std::make_unique<BlockTableRecord>();
    record->name = name;
    record->xrefType = overlay ? XrefType::Overlay : XrefType::Attachment;
    record->xrefPath = file.string();
    BlockTableRecord& added = db.addBlock(std::move(record));

    io::XrefLoadOptions options;
    options.translator = translator_.get();
    options.onProgress = [this](const std::string& message) {
        log(QString::fromStdString(message));
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    };
    const std::filesystem::path host =
        document_->filePath.empty()
            ? std::filesystem::path{}
            : std::filesystem::path{QString::fromStdString(document_->filePath).toStdWString()};
    io::XrefLoader loader{db};
    const io::XrefLoadResult result = loader.loadReference(added, host, options);
    for (const io::DxfDiagnostic& diagnostic : result.diagnostics) {
        log(QString::fromStdString(diagnostic.message));
    }
    for (const io::XrefNode& node : result.nodes) {
        if (node.blockHandle != added.handle) {
            continue;
        }
        if (node.status != XrefStatus::Resolved) {
            return node.problem.empty() ? "The reference could not be loaded." : node.problem;
        }
        log(tr("Reference %1 loaded: %2 object(s)")
                .arg(QString::fromStdString(name))
                .arg(node.entityCount));
    }
    *block = added.handle;
    return {};
}

void MainWindow::onReferenceManager() {
    if (document_ == nullptr) {
        return;
    }
    if (nonInteractive_) {
        // Said rather than silently skipped: a command that reports success
        // while doing nothing is worse than one that reports why it cannot.
        log(tr("! The reference manager needs the dialog, which is unavailable in a "
               "scripted run."));
        return;
    }
    ReferenceManagerDialog dialog{*document_, this};
    connect(&dialog, &ReferenceManagerDialog::referencesChanged, this, [this] {
        // A repathed image has to be decoded again, and the canvas has to be
        // told: the cache is keyed on the path, and the old one is still in it.
        // A PDF found for the first time also gets its page size now.
        if (fillUnderlayPageSizes(document_->db()) > 0) {
            document_->reindex();
        }
        canvas_->clearImageCache();
        canvas_->update();
        log(tr("Reference path updated."));
    });
    dialog.exec();
    onCommandStateChanged();
}

void MainWindow::onSelectionChanged() {
    if (document_ == nullptr) {
        return;
    }
    const std::vector<Handle>& selection = document_->selection();
    selectionLabel_->setText(selection.empty() ? QString{}
                                               : tr("%1 selected").arg(selection.size()));
    // The palette follows the selection; that is the whole interaction.
    properties_->refresh();

    // And so does the ribbon, when the selection is all of one kind.
    //
    // Mixed selections get no contextual tab: there is no honest answer to
    // "what would you like to do with these six things of four types", and
    // offering the first one's tools would be a guess dressed as an answer.
    std::string type;
    for (const Handle handle : selection) {
        const Entity* entity = document_->db().getEntity(handle);
        if (entity == nullptr) {
            continue;
        }
        const std::string current = entity->objectType();
        if (type.empty()) {
            type = current;
        } else if (type != current) {
            type.clear();
            break;
        }
    }
    ribbon_->setContextualSelection(QString::fromStdString(type), &document_->commands());
}

void MainWindow::onCommandStateChanged() {
    // A locked dimension belongs to the point it was typed for. Cancelling a
    // command, or finishing one, has to let it go -- otherwise the next point
    // asked for anywhere in the drawing is silently held at the old length.
    if (!canvas_->isEnteringDimension()) {
        canvas_->clearDimensionInput();
    }
    // Before the command line refreshes, so that when a command asks for text
    // it can be edited where it sits rather than at the bottom of the window.
    canvas_->syncInPlaceEditor();
    commandLine_->refresh();
    refreshCommandLog();
    refreshLayerPanel();
    refreshLayoutTabs();
    onSelectionChanged();
    updateWindowTitle();
    canvas_->update();
}

void MainWindow::log(const QString& message) {
    commandLog_->appendPlainText(message);
    // Also to the console when nobody is watching the window. A scripted run
    // that fails otherwise reports only an exit code, and the sentence saying
    // what actually went wrong scrolls past in a history panel that is never
    // shown to anyone.
    if (nonInteractive_) {
        std::fputs(message.toUtf8().constData(), stderr);
        std::fputc('\n', stderr);
    }
}

void MainWindow::updateWindowTitle() {
    const QString name =
        currentPath_.isEmpty() ? tr("Untitled") : QFileInfo{currentPath_}.fileName();
    const bool modified = document_ != nullptr && document_->isModified();
    // "Slate", not "Slate CAD". The suffix exists so that somebody looking for
    // drafting software finds this; a title bar is read by someone who has
    // already found it.
    setWindowTitle(QString{"%1%2 — %3"}.arg(name, modified ? "*" : "", kDisplayName));
}

}  // namespace cad::app
