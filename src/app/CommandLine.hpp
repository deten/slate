#pragma once

#include <QWidget>
#include <functional>
#include <vector>

#include "Document.hpp"

class QLabel;
class QLineEdit;

namespace cad::app {

/// The command line.
///
/// The primary input surface of a CAD application, not an afterthought: an
/// experienced drafter types far more than they click. It shows the running
/// command's prompt, accepts command names, coordinates, distances, angles and
/// keywords, and keeps a history the up arrow walks.
///
/// Everything typed here goes through the same `CommandExecutor::supply` that
/// canvas clicks use, which is what makes scripting and macro playback
/// identical to interactive use rather than a parallel path.
class CommandLine final : public QWidget {
    Q_OBJECT

public:
    explicit CommandLine(QWidget* parent = nullptr);

    void setDocument(Document* document);

    /// Offer a typed line to a held grip before treating it as a command.
    ///
    /// A grip edit is not a command -- nothing is running while one is held --
    /// so the command line would otherwise try to execute "250" and report that
    /// there is no such command. Given as a callback rather than a pointer to
    /// the canvas: the command line knowing about the drawing area would be a
    /// dependency in the wrong direction.
    void setDirectInput(std::function<bool(const QString&)> handler);

    /// The name of the grip mode being held, or empty when none is.
    ///
    /// other CAD software writes `** STRETCH **` while a grip is hot and changes the word
    /// as the space bar steps through the modes. A drafter coming from it reads
    /// that as "you are holding something, and this is what it will do".
    void setGripMode(std::function<QString()> mode);

    /// Refresh the prompt from the executor's current state.
    void refresh();

    /// Move keyboard focus to the input field.
    void focusInput();

    /// Start typing from somewhere else — the canvas, usually.
    ///
    /// A drafter types `m` `Enter` for MOVE without ever aiming at the command
    /// line, so a keystroke on the canvas has to arrive here as though it had
    /// been typed here. Focus moves with it, so the rest of the word follows
    /// without any further routing.
    void beginTypingWith(const QString& text);

    /// What is currently typed, for the cursor-following prompt to mirror.
    [[nodiscard]] QString currentText() const;

    /// Empty the input without submitting it. Used when a keystroke is spent
    /// somewhere else -- Tab locking a dimension box takes the number with it.
    void clearInput();

    /// The prompt as shown, e.g. "Specify next point".
    [[nodiscard]] QString promptText() const;

    /// Type a line and press Enter, as a user would.
    ///
    /// Exists so the application can be driven without a keyboard: scripted
    /// testing, and later macro playback, which the brief asks for. It goes
    /// through exactly the same parsing as typing does, so a script cannot
    /// reach a path the user could not.
    void submitLine(const QString& text);

Q_SIGNALS:
    /// Emitted after input is processed, so the canvas can repaint.
    void commandStateChanged();

    /// Emitted whenever the typed text or the prompt changes, so the
    /// cursor-following prompt can mirror it.
    void inputChanged();

    /// Shift+Space: step to the next object under the cursor.
    void selectionCycleRequested();


protected:
    /// Intercepts the keys QLineEdit would otherwise consume: the arrows walk
    /// the history and Escape cancels the running command.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void submit();

    /// Interpret typed text against what the command is currently asking for.
    ///
    /// The same text means different things in different states: "5" is a
    /// command name nowhere, a distance when a distance is wanted, and part of
    /// "5,3" when a point is. Parsing against the request rather than guessing
    /// is what makes typed input predictable.
    [[nodiscard]] bool supplyTyped(const QString& text);

    /// Parse "x,y" or "x,y,z" in either absolute or relative ("@dx,dy") form.
    [[nodiscard]] static std::optional<Vec3> parsePoint(const QString& text, bool& relative);

    Document* document_ = nullptr;
    std::function<bool(const QString&)> directInput_;
    std::function<QString()> gripMode_;
    QLabel* prompt_ = nullptr;
    QLineEdit* input_ = nullptr;

    std::vector<QString> history_;
    std::size_t historyCursor_ = 0;

    /// The request whose `editText` has already been placed in the box.
    ///
    /// Compared by address, which is enough: the executor holds one request at
    /// a time and replaces it wholesale. Without it, every repaint would reset
    /// the line and a note could not be edited at all — the characters would
    /// vanish as fast as they were typed.
    const InputRequest* seededRequest_ = nullptr;
};

}  // namespace cad::app
