#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace cad::app {

class ProjectIndex;

/// The set at a glance: its sheets in order, which one is open, which
/// files are missing, and whose background has moved on since the sheet
/// was last saved. Double-clicking a sheet opens it.
class ProjectPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ProjectPanel(QWidget* parent = nullptr);

    /// The project to show; null for none.
    void setIndex(ProjectIndex* index);
    /// Which drawing is open, so its sheet can be marked.
    void setCurrentFile(const QString& drawingPath);
    void refresh();

Q_SIGNALS:
    /// The user wants this drawing opened.
    void openSheetRequested(const QString& path);
    /// The user wants the project tool.
    void editRequested();
    /// The user wants the open sheet's background overlaid the office way.
    void attachBackgroundRequested();
    /// The user wants the set checked and the findings in the history.
    void checkRequested();

private:
    void onItemActivated(QTreeWidgetItem* item, int column);
    void onOpenClicked();

    ProjectIndex* index_ = nullptr;
    QString currentFile_;
    QLabel* title_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPushButton* open_ = nullptr;
    QLabel* note_ = nullptr;
};

}  // namespace cad::app
