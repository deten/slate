#include "RibbonBar.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <set>

#include "RibbonIcons.hpp"
#include "RibbonLayout.hpp"
#include "Theme.hpp"
#include "cad/command/CommandRegistry.hpp"

namespace cad::app {
namespace {

/// Fixed heights, so the ribbon cannot grow into the drawing area.
///
/// The quick-access strip rides in the tab row's left corner, and is kept well
/// under the height of the strip it sits in. A corner widget taller than the
/// tabs pushes the whole tab bar down, and the ribbon's height is fixed -- so
/// the pixels come out of the panel below, where three rows of small buttons
/// fit with nothing to spare and the third row loses its descenders.
constexpr int kTabStripHeight = 30;
constexpr int kQuickAccessHeight = 24;

/// A small button has to fit a line of the UI font with its descenders.
///
/// It was 16, which is shorter than the text it holds: "Bolt Circle" was
/// clipped through the middle and overlapped the button below it. Three rows of
/// text is the density that keeps a tab readable on a laptop, so the panel is
/// sized to three rows rather than the rows squeezed to fit the panel.
constexpr int kSmallButtonHeight = kRibbonSmallButtonHeight;
constexpr int kSmallRows = kRibbonSmallRows;
constexpr int kColumnSpacing = kRibbonColumnSpacing;
constexpr int kContentHeight = kRibbonContentHeight;
constexpr int kCaptionHeight = 15;
/// Height of the scroll bar under a tab whose groups are wider than the window.
///
/// Deliberately slim, and deliberately *always* accounted for. Letting it take
/// its space from the panel only when it appears would make the group captions
/// vanish on exactly the tabs that overflow -- which is how the first attempt
/// at this behaved.
constexpr int kOverflowBarHeight = 8;
constexpr int kPanelHeight = kContentHeight + kCaptionHeight + kOverflowBarHeight + 6;

/// A large button spans the content height and takes two lines of label.
constexpr int kLargeButtonWidth = 78;
constexpr int kLargeIconSize = 26;
constexpr int kSmallIconSize = 16;

/// Slack beyond a button's own size hint, for the style sheet's left padding.
///
/// Measured from the text alone it came out too narrow and every label longer
/// than its column elided: "Rectangle" as "Rec...gle", "Bolt Circle" as
/// "Bolt...rcle". A ribbon whose buttons cannot say what they do is a ribbon of
/// riddles, so the width comes from the widget's own hint plus room for the
/// padding the sheet adds, and the buttons are as wide as they need to be.
constexpr int kSmallButtonSlack = 12;

}  // namespace

// ------------------------------------------------------------------- panel

RibbonPanel::RibbonPanel(const QString& title, QWidget* parent) : QWidget{parent} {
    auto* outer = new QVBoxLayout{this};
    outer->setContentsMargins(6, 2, 6, 2);
    outer->setSpacing(2);

    auto* row = new QWidget{this};
    row->setFixedHeight(kContentHeight);
    content_ = new QHBoxLayout{row};
    content_->setContentsMargins(0, 0, 0, 0);
    content_->setSpacing(3);
    // Buttons hug the top-left, so a group with one column does not float its
    // contents in the middle of an otherwise empty panel.
    content_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    content_->addStretch(0);

    auto* caption = new QLabel{title, this};
    caption->setFixedHeight(kCaptionHeight);
    caption->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    caption->setStyleSheet(QString{"color: %1; font-size: 10px;"}
                               .arg(currentTheme().textMuted.name()));

    outer->addWidget(row, 0);
    outer->addWidget(caption, 0);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void RibbonPanel::squareUpColumn() {
    if (columnButtons_.empty()) {
        return;
    }
    int widest = 0;
    for (QToolButton* button : columnButtons_) {
        widest = std::max(widest, button->sizeHint().width());
    }
    const int width = widest + kSmallButtonSlack;
    for (QToolButton* button : columnButtons_) {
        button->setFixedWidth(width);
    }
    columnButtons_.clear();
}

void RibbonPanel::addCommand(const QString& name, const QString& label,
                             const QString& tooltip, bool large,
                             std::function<void()> action) {
    auto* button = new QToolButton{this};
    // A large button wraps at the first space rather than eliding: "Linear
    // Dimension" as "Li...on" is unreadable, and widening every button enough
    // to fit it on one line would make the row too wide to be worth having.
    button->setText(large ? QString{label}.replace(QChar{' '}, QChar{'\n'}) : label);
    button->setToolTip(tooltip.isEmpty() ? label : tooltip);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    // The ribbon never takes the keyboard. A drafter who clicks "Line" then
    // types a coordinate must have that coordinate reach the command line, not
    // a button that happens to be holding focus.
    button->setFocusPolicy(Qt::NoFocus);
    // Styled by name from the ribbon's own sheet rather than per button:
    // setting a style sheet on a widget re-polishes it, so a per-button one
    // fights every other property set on it.
    button->setObjectName(large ? "ribbonLarge" : "ribbonSmall");

    // A picture of what the command does, which is what makes a row of these
    // scannable rather than a wall of words. A large button carries it above
    // the label and a small one beside it, so a column of small buttons reads
    // down the left edge as a list.
    const Theme& theme = currentTheme();
    button->setIcon(ribbonIcon(name, label, large ? kLargeIconSize : kSmallIconSize, theme.text,
                               theme.accent));
    button->setIconSize(large ? QSize{kLargeIconSize, kLargeIconSize}
                              : QSize{kSmallIconSize, kSmallIconSize});
    button->setToolButtonStyle(large ? Qt::ToolButtonTextUnderIcon
                                     : Qt::ToolButtonTextBesideIcon);

    connect(button, &QToolButton::clicked, this, [action = std::move(action)] { action(); });

    if (large) {
        // Uniform width, widened only for a label that still does not fit after
        // wrapping. "References" is a single word, so there is no space to wrap
        // at and 78px elided it to "Ref...ces"; the alternative of widening
        // every button to the longest label wastes the row on the many short
        // ones.
        button->setFixedSize(std::max(kLargeButtonWidth, button->sizeHint().width()),
                             kContentHeight);
        // A large button interrupts a column of small ones, so the next small
        // button starts a fresh column rather than resuming a stale one.
        squareUpColumn();
        currentColumn_ = nullptr;
        content_->insertWidget(content_->count() - 1, button);
        return;
    }

    if (currentColumn_ == nullptr || static_cast<int>(columnButtons_.size()) >= kSmallRows) {
        squareUpColumn();
        currentColumn_ = new QWidget{this};
        currentColumn_->setFixedHeight(kContentHeight);
        auto* column = new QVBoxLayout{currentColumn_};
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(kColumnSpacing);
        column->setAlignment(Qt::AlignTop);
        content_->insertWidget(content_->count() - 1, currentColumn_);
    }
    button->setFixedHeight(kSmallButtonHeight);
    qobject_cast<QVBoxLayout*>(currentColumn_->layout())->addWidget(button);
    columnButtons_.push_back(button);
}

void RibbonPanel::addWidget(QWidget* widget) {
    widget->setParent(this);
    squareUpColumn();
    currentColumn_ = nullptr;
    content_->insertWidget(content_->count() - 1, widget);
}

void RibbonPanel::finish() {
    squareUpColumn();
}

// ------------------------------------------------------------------ ribbon

RibbonBar::RibbonBar(QWidget* parent) : QWidget{parent} {
    auto* outer = new QVBoxLayout{this};
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The quick-access strip shares the tab row rather than taking a row of its
    // own. Three stacked rows of chrome -- menus, file buttons, tabs -- cost
    // ninety pixels of drawing area and wrote "Draw", "Annotate", "Modify" and
    // "Insert" across two of them, which reads as two competing navigations
    // rather than one.
    auto* quickRow = new QWidget{this};
    quickRow->setFixedHeight(kQuickAccessHeight);
    quickAccess_ = new QHBoxLayout{quickRow};
    quickAccess_->setContentsMargins(6, 0, 8, 0);
    quickAccess_->setSpacing(1);
    quickAccess_->addStretch(0);

    tabs_ = new QTabWidget{this};
    tabs_->setDocumentMode(true);
    tabs_->setTabPosition(QTabWidget::North);
    // Neither does the tab strip. Left focusable, it takes the keyboard at
    // start-up and swallows the first command a user types.
    tabs_->setFocusPolicy(Qt::NoFocus);
    tabs_->tabBar()->setFocusPolicy(Qt::NoFocus);

    // Left of the tabs, on the same line. A corner widget is sized by the tab
    // widget to the height of the tab bar, which is what keeps the two halves
    // of the row sitting on the same baseline.
    tabs_->setCornerWidget(quickRow, Qt::TopLeftCorner);

    outer->addWidget(tabs_);

    // One sheet for every button in the ribbon.
    //
    // A large button is the one a hand goes to without looking, so it is given
    // weight the eye can find at a glance; a small one is read. Both need a
    // hover state — a flat label that does nothing when the pointer crosses it
    // does not read as something you can press.
    const Theme& theme = currentTheme();
    setStyleSheet(QString{R"(
        /* No vertical padding. A small button is 22px tall, and the
           application sheet's 3px above and below left 16px of content box for
           a 13px font whose line box is taller than that -- so every label in
           every column lost the bottom two rows of its glyphs, and "Rectangle"
           lost the descender of its g entirely. The height is the budget; the
           text gets all of it. */
        QToolButton#ribbonSmall {
            text-align: left;
            padding: 0px 6px 0px 6px;
            border: none;
            border-radius: 3px;
            color: %1;
        }
        QToolButton#ribbonLarge {
            border: none;
            border-radius: 4px;
            font-weight: 600;
            color: %1;
        }
        QToolButton#ribbonSmall:hover, QToolButton#ribbonLarge:hover {
            background: %2;
        }
        QToolButton#ribbonSmall:pressed, QToolButton#ribbonLarge:pressed {
            background: %3;
        }
        /* On, Freeze and Lock are toggles, and a toggle whose state cannot be
           read is a switch with no label on it. Styling a tool button at all
           takes its checked look away from the platform, so the sheet has to
           give it back. */
        QToolButton#ribbonSmall:checked {
            background: %3;
            color: white;
        }
        QToolButton#quickAccess {
            border: none;
            border-radius: 3px;
            padding: 3px 5px;
            color: %1;
        }
        QToolButton#quickAccess:hover { background: %2; }
        QToolButton#quickAccess:pressed { background: %3; }
        /* The dropdown arrow beside the menu button is noise: the button is
           already the only thing on the row that opens anything, and at this
           size the arrow crowds the icon rather than explaining it. */
        QToolButton#quickAccess::menu-indicator { image: none; width: 0; }
    )"}
                      .arg(theme.text.name(), theme.panelAlternate.name(),
                           theme.accent.name()));

    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // Sized to its content rather than its content squeezed to fit. A ribbon
    // that grows with what is in it would eat the drawing area, which is the
    // one thing the window is actually for — so the content is bounded instead,
    // at three rows of buttons.
    setFixedHeight(kPanelHeight + kTabStripHeight);
}

void RibbonBar::setHomePanel(const QString& title, QWidget* content) {
    homePanel_ = new RibbonPanel{title, this};
    homePanel_->addWidget(content);
    homePanel_->finish();
    homePanel_->hide();
}

void RibbonBar::addQuickAccess(QWidget* widget) {
    widget->setParent(this);
    // Inserted before the trailing stretch, so items pack to the left.
    quickAccess_->insertWidget(quickAccess_->count() - 1, widget);
}

bool RibbonBar::showTab(const QString& title) {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->tabText(i) == title) {
            tabs_->setCurrentIndex(i);
            return true;
        }
    }
    return false;
}

void RibbonBar::clearContextualTab() {
    if (contextualPage_ == nullptr) {
        return;
    }
    const int index = tabs_->indexOf(contextualPage_);
    const bool wasCurrent = index >= 0 && index == tabs_->currentIndex();
    if (index >= 0) {
        tabs_->removeTab(index);
    }
    delete contextualPage_;
    contextualPage_ = nullptr;
    contextualType_.clear();

    // Removing the current tab leaves Qt on whichever one is now at that index
    // — which after a contextual tab at the end is the last fixed tab. A
    // drafter who was on Home, selected a table and then deselected would find
    // themselves on Tools, having asked for neither.
    if (wasCurrent && !tabBeforeContextual_.isEmpty()) {
        static_cast<void>(showTab(tabBeforeContextual_));
    }
    tabBeforeContextual_.clear();
}

void RibbonBar::setContextualSelection(const QString& objectType,
                                       const CommandRegistry* registry) {
    if (objectType == contextualType_) {
        return;  // Nothing changed; rebuilding would flicker for no reason.
    }

    const std::string type = objectType.toStdString();
    const std::string title = contextualTitleFor(type);
    if (registry == nullptr || title.empty()) {
        clearContextualTab();
        return;
    }

    const bool hadTab = contextualPage_ != nullptr;
    // Noted before the tab goes, so a selection that changes from one
    // contextual type to another still remembers where the user started.
    const QString before =
        hadTab ? tabBeforeContextual_
               : (tabs_->currentIndex() >= 0 ? tabs_->tabText(tabs_->currentIndex())
                                             : QString{});
    clearContextualTab();
    tabBeforeContextual_ = before;

    auto* page = new QWidget{tabs_};
    auto* layout = new QHBoxLayout{page};
    layout->setContentsMargins(4, 1, 4, 1);
    layout->setSpacing(3);
    layout->addStretch(1);

    bool any = false;
    for (const RibbonGroup& group : contextualGroupsFor(type)) {
        std::vector<std::pair<const CommandDefinition*, bool>> commands;
        for (const RibbonSlot& slot : group.slots) {
            if (const CommandDefinition* command = registry->lookup(slot.command)) {
                commands.emplace_back(command, slot.size == RibbonSize::Large);
            }
        }
        if (!commands.empty()) {
            addGroup(page, QString::fromStdString(group.title), commands);
            any = true;
        }
    }
    if (!any) {
        delete page;
        return;
    }

    contextualPage_ = page;
    contextualType_ = objectType;
    const int index = tabs_->addTab(page, QString::fromStdString(title));
    // Marked out from the fixed tabs, because a tab that comes and goes has to
    // look like one — otherwise its arrival reads as the ribbon rearranging
    // itself.
    tabs_->tabBar()->setTabTextColor(index, currentTheme().accent);

    // Raised only when the tab is new. Doing it on every selection would move
    // the ribbon under the hand of somebody who was about to click something
    // else on it.
    if (!hadTab) {
        tabs_->setCurrentIndex(index);
    }
}

void RibbonBar::addGroup(QWidget* page, const QString& title,
                         const std::vector<std::pair<const CommandDefinition*, bool>>& commands) {
    if (commands.empty()) {
        return;
    }
    auto* panel = new RibbonPanel{title, page};
    for (const auto& [command, large] : commands) {
        const QString name = QString::fromStdString(command->name);
        panel->addCommand(name, QString::fromStdString(command->title),
                          QString::fromStdString(command->description), large,
                          [this, name] { Q_EMIT commandTriggered(name); });
    }
    panel->finish();

    auto* layout = qobject_cast<QHBoxLayout*>(page->layout());
    layout->insertWidget(layout->count() - 1, panel);

    // A rule between groups. Without it the captions are the only boundary, and
    // a caption sits *below* its group — so the eye has to read the labels to
    // find out where one group ends.
    auto* divider = new QFrame{page};
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Plain);
    divider->setFixedWidth(1);
    divider->setStyleSheet(
        QString{"background: %1; border: none;"}.arg(currentTheme().panelBorder.name()));
    layout->insertWidget(layout->count() - 1, divider);
}

void RibbonBar::rebuild(const CommandRegistry* registry) {
    const QString previous =
        tabs_->currentIndex() >= 0 ? tabs_->tabText(tabs_->currentIndex()) : QString{};

    // Rescue the caller's panel before the page holding it is deleted.
    if (homePanel_ != nullptr) {
        homePanel_->setParent(this);
        homePanel_->hide();
    }

    while (tabs_->count() > 0) {
        QWidget* page = tabs_->widget(0);
        tabs_->removeTab(0);
        delete page;
    }
    if (registry == nullptr) {
        return;
    }

    // A page is a row of groups inside a scroll area, and the scroll area is
    // the point: a tab whose groups are wider than the window used to have the
    // surplus simply cut off at the frame. Nothing said so -- the Home tab lost
    // its annotation tools on a 1600-pixel window and looked deliberate. A
    // command a drafter cannot reach is a command they do not have, so the row
    // scrolls rather than truncates.
    //
    // Returns the row; the scroll area is what goes into the tab bar, and
    // `pageOf` recovers one from the other.
    const auto newPage = [this] {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout{row};
        layout->setContentsMargins(4, 1, 4, 1);
        layout->setSpacing(3);
        // Panels hang from the top of the page.
        //
        // Left to centre themselves they sat a few pixels lower whenever the
        // tab's groups fitted the window, because with nothing to scroll the
        // overflow bar's eight pixels came back and the panels shared them out
        // -- which pushed the third row of small buttons into the caption and
        // sliced the descenders off "Lock" and "Offset" on a wide screen only.
        layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        layout->addStretch(1);

        auto* scroll = new QScrollArea{tabs_};
        scroll->setWidget(row);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        // Vertically the row is exactly the height it needs, so a vertical bar
        // could only ever be an artefact of rounding.
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setFocusPolicy(Qt::NoFocus);
        scroll->setStyleSheet(QString{R"(
            QScrollBar:horizontal { height: %1px; background: transparent; margin: 0; }
            QScrollBar::handle:horizontal { background: %2; border-radius: %3px;
                                            min-width: 40px; }
            QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
            QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal
                { background: transparent; }
        )"}
                                  .arg(kOverflowBarHeight)
                                  .arg(currentTheme().panelBorder.name())
                                  .arg(kOverflowBarHeight / 2));
        row->setProperty("ribbonScroller", QVariant::fromValue(static_cast<QWidget*>(scroll)));
        return row;
    };

    // The widget to hand the tab bar for a page built by `newPage`.
    const auto containerFor = [](QWidget* page) -> QWidget* {
        auto* scroll = page->property("ribbonScroller").value<QWidget*>();
        return scroll != nullptr ? scroll : page;
    };

    // Every command the curated layout places, so what is left over can be
    // found and given a home rather than disappearing.
    std::set<std::string> placed;
    std::map<QString, QWidget*> pages;

    for (const RibbonTab& tab : ribbonLayout()) {
        auto* page = newPage();
        const QString tabTitle = QString::fromStdString(tab.title);

        if (tabTitle == "Home" && homePanel_ != nullptr) {
            homePanel_->setParent(page);
            homePanel_->show();
            auto* layout = qobject_cast<QHBoxLayout*>(page->layout());
            layout->insertWidget(layout->count() - 1, homePanel_);
            auto* divider = new QFrame{page};
            divider->setFrameShape(QFrame::VLine);
            divider->setFixedWidth(1);
            divider->setStyleSheet(QString{"background: %1; border: none;"}.arg(
                currentTheme().panelBorder.name()));
            layout->insertWidget(layout->count() - 1, divider);
        }

        for (const RibbonGroup& group : tab.groups) {
            std::vector<std::pair<const CommandDefinition*, bool>> commands;
            for (const RibbonSlot& slot : group.slots) {
                const CommandDefinition* command = registry->lookup(slot.command);
                if (command == nullptr) {
                    // Named in the layout and not registered: a command that
                    // has not been written yet, or one behind a build option.
                    // Skipped silently, because an empty button would be worse.
                    continue;
                }
                commands.emplace_back(command, slot.size == RibbonSize::Large);
                placed.insert(command->name);
            }
            addGroup(page, QString::fromStdString(group.title), commands);
        }

        pages.emplace(tabTitle, page);
        tabs_->addTab(containerFor(page), tabTitle);
    }

    // Whatever the table does not mention — a plugin's command, or one added
    // since — grouped by its category on the tab that category maps to. This is
    // what stops the curated layout from being a way to lose commands.
    for (const auto& [category, commands] : registry->byCategory()) {
        // Split by where the command came from. A plugin's command and one this
        // application simply has not curated yet are different facts about the
        // drawing environment, and filing both under "More" says neither: the
        // drafter cannot tell which button belongs to the add-in they
        // installed, and the gap in this application's own layout is hidden
        // rather than shown.
        std::vector<std::pair<const CommandDefinition*, bool>> extras;
        std::vector<std::pair<const CommandDefinition*, bool>> fromPlugins;
        std::vector<std::pair<const CommandDefinition*, bool>> fromMacros;
        for (const CommandDefinition* command : commands) {
            if (placed.contains(command->name)) {
                continue;
            }
            if (command->fromMacro) {
                fromMacros.emplace_back(command, false);
            } else if (command->fromPlugin) {
                fromPlugins.emplace_back(command, false);
            } else {
                extras.emplace_back(command, false);
            }
        }
        if (extras.empty() && fromPlugins.empty() && fromMacros.empty()) {
            continue;
        }
        const QString tabTitle = QString::fromStdString(tabForCategory(category));
        const auto page = pages.find(tabTitle);
        if (page == pages.end()) {
            continue;
        }
        // A leftover group named after the tab it is on reads as a duplicate
        // heading — a "Draw" group inside the Draw tab says nothing.
        const QString category_ = QString::fromStdString(category);
        if (!extras.empty()) {
            addGroup(page->second, category_ == tabTitle ? QObject::tr("More") : category_,
                     extras);
        }
        if (!fromPlugins.empty()) {
            addGroup(page->second, QObject::tr("Add-ins"), fromPlugins);
        }
        if (!fromMacros.empty()) {
            addGroup(page->second, QObject::tr("My Commands"), fromMacros);
        }
    }

    // Restore by name, not index: the tab set changes with the document and an
    // index would silently move the user to a different tab.
    // A rebuild deletes every page, the contextual one included.
    contextualPage_ = nullptr;
    contextualType_.clear();

    if (!previous.isEmpty()) {
        // A tab that has gone is not an error: the set changes with the
        // document, and the first tab is the right place to land.
        static_cast<void>(showTab(previous));
    }
}

}  // namespace cad::app
