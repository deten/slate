#pragma once

#include <QDialog>

#include "cad/db/SelectionFilter.hpp"

class QComboBox;
class QLabel;
class QLineEdit;

namespace cad::app {

class Document;

/// QSELECT — build a selection by querying properties.
///
/// A dialog rather than a prompt sequence, which is a deliberate exception to
/// this application's keyboard-first habit. A quick select is one *compound*
/// question — type, property, operator, value, and what to do with the result —
/// and asking it as five sequential prompts makes changing the third answer
/// mean starting over. other CAD software reached the same conclusion.
///
/// The choices are built from the property registry, so the type list is the
/// types actually present in this drawing and the property list is whatever
/// those types expose — including properties contributed by a plugin.
class QuickSelectDialog final : public QDialog {
    Q_OBJECT

public:
    QuickSelectDialog(Document& document, QWidget* parent = nullptr);

    /// The filter the user built. Only meaningful after `exec()` returns
    /// Accepted.
    [[nodiscard]] SelectionFilter filter() const;

private:
    /// Repopulate the property list for the chosen object type, and the value
    /// editor for the chosen property.
    void refreshProperties();
    void refreshValueEditor();

    /// Count the matches and show it, so the user can tell a query that found
    /// nothing from one they have not run yet.
    void refreshPreview();

    /// Object types present in this drawing, rather than every type that could
    /// exist. A list offering "Spline" in a drawing with no splines is a list
    /// that wastes the user's time.
    [[nodiscard]] std::vector<std::string> typesInDrawing() const;

    Document& document_;
    QComboBox* type_ = nullptr;
    QComboBox* property_ = nullptr;
    QComboBox* comparison_ = nullptr;
    QComboBox* mode_ = nullptr;
    QLineEdit* value_ = nullptr;
    QComboBox* valueChoice_ = nullptr;
    QLabel* preview_ = nullptr;
};

}  // namespace cad::app
