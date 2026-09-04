#pragma once

#include <QWidget>

class QLabel;

namespace cad::app {

/// The prompt that follows the cursor.
///
/// other CAD software calls this dynamic input, and it exists because a drafter's eyes are
/// on the geometry, not on a bar at the bottom of the window. Typing `m` should
/// put "MOVE" under the cursor, not somewhere the user has to look away to
/// find.
///
/// A display only: the text lives in the command line, which owns parsing and
/// history. Two widgets showing one buffer, rather than two buffers — anything
/// else drifts the moment a command supplies a default.
class DynamicInput final : public QWidget {
    Q_OBJECT

public:
    explicit DynamicInput(QWidget* parent = nullptr);

    /// Show `prompt` and `text` near `cursor`, in the parent's coordinates.
    ///
    /// Position is a request, not an instruction: the widget keeps itself
    /// inside the parent, because a prompt half off the edge of the canvas is
    /// worse than one a few pixels from where it was asked to go.
    void showAt(const QPoint& cursor, const QString& prompt, const QString& text);


    void hideInput();

private:
    /// Position and show the box, with whatever text it has been given.
    void place(const QPoint& cursor, const QString& prompt, const QString& text);

    QLabel* prompt_ = nullptr;
    QLabel* text_ = nullptr;
};

}  // namespace cad::app
