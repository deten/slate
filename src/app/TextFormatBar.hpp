#pragma once

#include <QWidget>

class QToolButton;

namespace cad::app {

/// The formatting controls that follow an in-place text edit.
///
/// It sits above the editor rather than in the ribbon, because the thing it acts
/// on is a *selection inside the text* — and a control for that has to be within
/// reach of the words being selected, not at the top of the window. It is also
/// only meaningful while an edit is open, which is exactly when it is shown.
///
/// **Nothing here takes the keyboard.** A button that stole focus would clear
/// the selection it was about to format, which is the one thing this must not
/// do.
class TextFormatBar final : public QWidget {
    Q_OBJECT

public:
    explicit TextFormatBar(QWidget* parent = nullptr);

    /// Reflect the formatting at the cursor, so the buttons say what is true of
    /// the text rather than what was last pressed.
    void showState(bool bold, bool italic, bool underline);

Q_SIGNALS:
    void boldToggled(bool on);
    void italicToggled(bool on);
    void underlineToggled(bool on);
    /// Multiply the selection's height by this much.
    void heightScaled(double factor);
    void colorPicked(const QColor& color);
    /// Return every run in the selection to the entity's own formatting.
    void formattingCleared();

private:
    QToolButton* bold_ = nullptr;
    QToolButton* italic_ = nullptr;
    QToolButton* underline_ = nullptr;
    /// Set while the buttons are being updated to match the cursor, so the
    /// resulting `toggled` signals do not read as the user pressing them.
    bool syncing_ = false;
};

}  // namespace cad::app
