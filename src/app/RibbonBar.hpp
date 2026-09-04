#pragma once

#include <QWidget>
#include <functional>
#include <string>
#include <vector>

class QHBoxLayout;
class QTabWidget;
class QToolButton;

namespace cad {
class CommandRegistry;
struct CommandDefinition;
}

namespace cad::app {

/// A group of related buttons within a ribbon tab, with a caption beneath.
///
/// The caption is what makes a ribbon navigable rather than a wall of labels:
/// "Lines", "Curves", "Regions" tell the eye where to go before it reads a
/// single button.
class RibbonPanel final : public QWidget {
    Q_OBJECT

public:
    RibbonPanel(const QString& title, QWidget* parent = nullptr);

    /// Add a button. `large` gives it the full panel height, for the two or
    /// three commands in a group used far more than the rest.
    void addCommand(const QString& name, const QString& label, const QString& tooltip,
                    bool large,
                    std::function<void()> action);

    /// Add an arbitrary widget — a combo box, a toggle — beside the buttons.
    void addWidget(QWidget* widget);

    /// Settle the column widths. Must be called once everything is added.
    ///
    /// Small buttons stack three to a column, and a column whose buttons are
    /// each their own width is a ragged edge that reads as broken. They can
    /// only be squared up once the widest of them is known, which is not until
    /// the last one has arrived.
    void finish();

private:
    /// Give every button in the column in progress the width of the widest.
    void squareUpColumn();

    QHBoxLayout* content_ = nullptr;
    QWidget* currentColumn_ = nullptr;
    std::vector<QToolButton*> columnButtons_;
};

/// The ribbon.
///
/// Replaces a toolbar because a CAD application has far too many commands for
/// one row. Tabs split them by *what you are doing*, and a captioned group
/// inside a tab gives the eye a second level of structure — with a rule between
/// groups, so the boundary is visible without being read.
///
/// The arrangement comes from `ribbonLayout()`, a curated table, rather than
/// from the command registry's categories. See RibbonLayout.hpp for why those
/// are different questions. Commands the table does not mention still appear,
/// under their category, so a plugin is always reachable.
class RibbonBar final : public QWidget {
    Q_OBJECT

public:
    explicit RibbonBar(QWidget* parent = nullptr);

    /// Rebuild from a registry. Safe to call repeatedly; the current tab is
    /// preserved by name so switching documents does not move the user.
    void rebuild(const CommandRegistry* registry);

    /// Add a widget to the always-visible strip left of the tabs, for things
    /// that are not commands — the file actions.
    void addQuickAccess(QWidget* widget);

    /// Put a caller-owned panel at the head of the Home tab.
    ///
    /// For controls that are not commands and so cannot come from the registry
    /// — the layer strip. It outlives `rebuild()`, which is the point: it is
    /// wired to the document and rebuilding it would drop those connections.
    void setHomePanel(const QString& title, QWidget* content);

    /// Show the tab of the given name, if there is one.
    [[nodiscard]] bool showTab(const QString& title);

    /// Show a tab for what is currently selected, or remove it when nothing
    /// worth offering is selected.
    ///
    /// `objectType` is an entity's own type name; an empty one, or a mixed
    /// selection, removes the tab. It is appended after the fixed tabs and
    /// raised the first time it appears — but not on every selection, because a
    /// ribbon that moves under the hand each time something is clicked is a
    /// ribbon nobody can aim at.
    void setContextualSelection(const QString& objectType, const CommandRegistry* registry);

Q_SIGNALS:
    /// A command button was pressed.
    void commandTriggered(const QString& name);

private:
    /// Add a captioned group to a page, followed by a dividing rule.
    void addGroup(QWidget* page, const QString& title,
                  const std::vector<std::pair<const CommandDefinition*, bool>>& commands);

    /// Remove the contextual tab, if one is up.
    void clearContextualTab();

    QTabWidget* tabs_ = nullptr;
    /// The type the contextual tab is currently for, so an unchanged selection
    /// does not rebuild it.
    QString contextualType_;
    QWidget* contextualPage_ = nullptr;
    /// The tab that was current when the contextual one was raised, so
    /// dismissing it returns the user where they were rather than wherever Qt
    /// happens to land.
    QString tabBeforeContextual_;
    QHBoxLayout* quickAccess_ = nullptr;
    /// Reparented out of the Home page before a rebuild deletes it, and back
    /// in afterwards, so it survives.
    RibbonPanel* homePanel_ = nullptr;
};

}  // namespace cad::app
