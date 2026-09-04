#pragma once

#include <QDialog>
#include <vector>

#include "cad/io/XrefLoader.hpp"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace cad::app {

class Document;

/// The reference manager: every file this drawing depends on, and whether it is
/// there.
///
/// The application could already say that an image was missing, and could say
/// exactly which path it looked for. What it could not do was anything about
/// it, which makes the report a complaint rather than a diagnosis. A drawing
/// that arrives from another office with its images on a drive letter nobody
/// here has is the ordinary case, not the exceptional one.
///
/// Repathing writes the *stored* path, and stores it relative to the drawing
/// when the new file is inside the drawing's own folder. Writing the absolute
/// path would fix the file on this machine and break it for everyone else,
/// which is the failure the whole resolution mechanism exists to avoid.
class ReferenceManagerDialog final : public QDialog {
    Q_OBJECT

public:
    ReferenceManagerDialog(Document& document, QWidget* parent = nullptr);

Q_SIGNALS:
    /// A path was changed and the drawing needs redrawing. Emitted rather than
    /// acted on, because the dialog has no business reaching into the canvas.
    void referencesChanged();

private:
    void reload();
    void onSelectionChanged();
    void onLocate();

    /// The definition currently selected, or a null handle.
    [[nodiscard]] Handle selectedDefinition() const;

    Document& document_;
    QTreeWidget* tree_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* locate_ = nullptr;
    std::vector<io::AttachedFileNode> nodes_;
};

}  // namespace cad::app
