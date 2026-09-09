#pragma once

#include <QDialog>
#include <QString>

#include "cad/project/Project.hpp"

class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QLabel;

namespace cad::io {
class DwgTranslator;
}

namespace cad::app {

class ProjectIndex;

/// The project tool: make a project file, or change one.
///
/// A set of sheets described in one place, made by Slate rather than typed:
/// the sheets can be listed from the drawings in the folder, each one's
/// number and title read from its title block, and the file is written
/// beside them as `project.slate`. Nothing about the drawings changes.
class ProjectDialog final : public QDialog {
    Q_OBJECT

public:
    /// Edit the project in `index`, or start one in `folder` when none is
    /// open.
    ProjectDialog(ProjectIndex& index, const QString& folder, QWidget* parent = nullptr);

    /// The project as it stands in the form.
    [[nodiscard]] Project project() const;
    /// Where the file was, or will be, written.
    [[nodiscard]] QString path() const noexcept { return path_; }

private Q_SLOTS:
    void onAddSheet();
    void onRemoveSheet();
    void onAddBackground();
    void onRemoveBackground();
    void onScan();
    void onMakeFiles();
    void onSave();

private:
    /// Write the form to the project file, opening it in the index.
    bool store();
    void fill(const Project& project);
    void addSheetRow(const ProjectSheet& sheet);
    void addBackgroundRow(const ProjectBackground& background);

    ProjectIndex& index_;
    QString folder_;
    QString path_;
    QLineEdit* name_ = nullptr;
    QLineEdit* discipline_ = nullptr;
    QLineEdit* template_ = nullptr;
    QPlainTextEdit* notes_ = nullptr;
    QTableWidget* sheets_ = nullptr;
    QTableWidget* backgrounds_ = nullptr;
    QLabel* status_ = nullptr;
};

}  // namespace cad::app
