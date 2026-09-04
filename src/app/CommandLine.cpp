#include "CommandLine.hpp"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>

#include "Theme.hpp"
#include "cad/command/TypedInput.hpp"
#include "cad/math/Angle.hpp"

namespace cad::app {
namespace {

/// Match typed text against a request's keywords.
///
/// other CAD software's convention is that the capitalized prefix of a keyword is enough:
/// "C" selects "Close". Matching the shortcut first, then a case-insensitive
/// prefix of the full word, reproduces that.
[[nodiscard]] const CommandKeyword* matchKeyword(const InputRequest& request,
                                                 const QString& text) {
    const QString upper = text.trimmed().toUpper();
    if (upper.isEmpty()) {
        return nullptr;
    }

    for (const CommandKeyword& keyword : request.keywords) {
        if (QString::fromStdString(keyword.shortcut).toUpper() == upper) {
            return &keyword;
        }
    }
    for (const CommandKeyword& keyword : request.keywords) {
        if (QString::fromStdString(keyword.name).toUpper().startsWith(upper)) {
            return &keyword;
        }
    }
    return nullptr;
}

/// Build the prompt line, including the keyword hints other CAD software shows in
/// brackets. Discoverability without a dialog.
[[nodiscard]] QString formatPrompt(const InputRequest& request) {
    QString text = QString::fromStdString(request.prompt);

    if (!request.keywords.empty()) {
        QStringList names;
        for (const CommandKeyword& keyword : request.keywords) {
            names << QString::fromStdString(keyword.name);
        }
        text += QString{" [%1]"}.arg(names.join('/'));
    }
    if (request.defaultNumber.has_value()) {
        text += QString{" <%1>"}.arg(*request.defaultNumber);
    }
    return text + ": ";
}

}  // namespace

CommandLine::CommandLine(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout{this};
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(8);

    prompt_ = new QLabel{tr("Command"), this};
    prompt_->setStyleSheet(
        QString{"color: %1; font-weight: 600;"}.arg(currentTheme().textMuted.name()));

    input_ = new QLineEdit{this};
    // A line edit takes drops of its own and would paste a dropped file's path
    // as a command. Declining lets the drop reach the window, which opens it.
    input_->setAcceptDrops(false);
    input_->setPlaceholderText(tr("Type a command, or pick a point…"));
    input_->installEventFilter(this);


    layout->addWidget(prompt_);
    layout->addWidget(input_, 1);

    connect(input_, &QLineEdit::returnPressed, this, &CommandLine::submit);
    connect(input_, &QLineEdit::textChanged, this, &CommandLine::inputChanged);
}

void CommandLine::setDocument(Document* document) {
    document_ = document;
    refresh();
}

void CommandLine::focusInput() {
    input_->setFocus(Qt::OtherFocusReason);
}

void CommandLine::beginTypingWith(const QString& text) {
    // Focus follows the first keystroke, so the rest of the word arrives here
    // without the canvas having to forward every character.
    input_->setFocus(Qt::OtherFocusReason);
    input_->setText(text);
    input_->setCursorPosition(static_cast<int>(text.size()));
    Q_EMIT inputChanged();
}

QString CommandLine::currentText() const {
    return input_->text();
}

QString CommandLine::promptText() const {
    return prompt_->text();
}

void CommandLine::refresh() {
    if (document_ == nullptr) {
        prompt_->setText(tr("Command"));
        Q_EMIT inputChanged();
        return;
    }

    const InputRequest* request = document_->executor().currentRequest();
    const QString gripMode = gripMode_ ? gripMode_() : QString{};
    if (!gripMode.isEmpty()) {
        // other CAD software's own words, because a drafter arriving from it already knows
        // what they mean.
        prompt_->setText(tr("** %1 **").arg(gripMode));
    } else {
        prompt_->setText(request == nullptr ? tr("Command: ") : formatPrompt(*request));
    }

    // A request that asks the user to *amend* a value puts that value in the
    // box, selected, so typing replaces it and an arrow key edits it. Seeded
    // once per request rather than on every refresh, or a repaint halfway
    // through editing a note would throw away what had been typed.
    if (request != nullptr && request->editText.has_value() && request != seededRequest_) {
        input_->setText(QString::fromStdString(*request->editText));
        input_->selectAll();
        // Seeded, not focused. Taking the keyboard here would fight the in-place
        // editor on the canvas for it, and the fight is invisible: the editor
        // opens, loses focus a moment later, and the edit commits itself with
        // nothing typed.
    }
    seededRequest_ = request;

    Q_EMIT inputChanged();
}

std::optional<Vec3> CommandLine::parsePoint(const QString& text, bool& relative) {
    QString body = text.trimmed();
    relative = false;

    // "@dx,dy" is relative to the last point — how a drafter enters a run of
    // known offsets without computing absolute coordinates.
    if (body.startsWith('@')) {
        relative = true;
        body.remove(0, 1);
    }

    const QStringList parts = body.split(',', Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts.size() > 3) {
        return std::nullopt;
    }

    bool okX = false;
    bool okY = false;
    const double x = parts[0].trimmed().toDouble(&okX);
    const double y = parts[1].trimmed().toDouble(&okY);
    if (!okX || !okY) {
        return std::nullopt;
    }

    double z = 0.0;
    if (parts.size() == 3) {
        bool okZ = false;
        z = parts[2].trimmed().toDouble(&okZ);
        if (!okZ) {
            return std::nullopt;
        }
    }
    return Vec3{x, y, z};
}

bool CommandLine::supplyTyped(const QString& text) {
    CommandExecutor& executor = document_->executor();
    const InputRequest* request = executor.currentRequest();
    if (request == nullptr) {
        return false;
    }
    // Parsed by the core, so that what a word means does not depend on which
    // part of the application is reading it. This used to be spelled out here,
    // and anything that was not this widget -- a script, an agent -- had to
    // reimplement it and could disagree about "@30,40".
    const std::optional<InputValue> value =
        parseTypedInput(*request, text.toStdString(), document_->selection());
    if (!value) {
        return false;
    }
    executor.supply(*value);
    return true;
}

bool CommandLine::eventFilter(QObject* watched, QEvent* event) {
    // Ctrl+Z did nothing to the drawing, and this is why.
    //
    // The command line holds the keyboard at all times, and a QLineEdit claims
    // Ctrl+Z to undo its own text. It does that by *accepting* the
    // shortcut-override event, which tells Qt not to fire the application's
    // Undo action — so the most important keystroke in the program was being
    // spent rewinding a one-line text box that is cleared on every command.
    //
    // Declining the override here hands the key back to the drawing. Nothing is
    // lost: the up arrow recalls command history, which is what this field
    // actually offers.
    if (watched == input_ && event->type() == QEvent::ShortcutOverride) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->matches(QKeySequence::Undo) || keyEvent->matches(QKeySequence::Redo)) {
            keyEvent->ignore();
            return true;
        }
    }

    if (watched != input_ || event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }

    auto* keyEvent = static_cast<QKeyEvent*>(event);
    switch (keyEvent->key()) {
        case Qt::Key_Up:
            if (historyCursor_ > 0) {
                --historyCursor_;
                input_->setText(history_[historyCursor_]);
                input_->selectAll();
            }
            return true;

        case Qt::Key_Down:
            if (historyCursor_ + 1 < history_.size()) {
                ++historyCursor_;
                input_->setText(history_[historyCursor_]);
                input_->selectAll();
            } else {
                historyCursor_ = history_.size();
                input_->clear();
            }
            return true;

        case Qt::Key_Escape:
            if (document_ != nullptr && document_->executor().isRunning()) {
                document_->executor().supply(InputValue::makeCancel());
                document_->reindex();
                refresh();
                Q_EMIT commandStateChanged();
            } else if (document_ != nullptr && !document_->selection().empty()) {
                // Escape with no command clears the selection, as other CAD software does.
                document_->clearSelection();
                Q_EMIT commandStateChanged();
            }
            input_->clear();
            return true;

        case Qt::Key_Space:
            // Shift+Space steps through overlapping objects under the cursor,
            // as other CAD software does. Routed from here because the command line holds
            // the keyboard at all times, so the canvas never sees a key.
            if ((keyEvent->modifiers() & Qt::ShiftModifier) != 0) {
                Q_EMIT selectionCycleRequested();
                return true;
            }
            // Space ends a command name, exactly as Enter does: `m` space runs
            // MOVE. It is only an ordinary space when a command is asking for
            // *text*, which is the one context where a space is content.
            if (document_ != nullptr) {
                const InputRequest* request = document_->executor().currentRequest();
                if (request != nullptr && request->kind == InputRequest::Kind::Text) {
                    break;
                }
            }
            submit();
            return true;

        case Qt::Key_Delete:
            // Delete erases the selection. It lives here rather than on the
            // canvas because the command line holds the keyboard; on the
            // canvas it would only fire when nothing was being typed, which is
            // exactly when the user cannot tell which widget has focus.
            if (input_->text().isEmpty() && document_ != nullptr &&
                !document_->executor().isRunning() && !document_->selection().empty()) {
                document_->executor().execute("ERASE");
                document_->clearSelection();
                document_->reindex();
                refresh();
                Q_EMIT commandStateChanged();
                return true;
            }
            break;

        default:
            break;
    }
    return QWidget::eventFilter(watched, event);
}

void CommandLine::submitLine(const QString& text) {
    input_->setText(text);
    submit();
}

void CommandLine::submit() {
    if (document_ == nullptr) {
        return;
    }

    const QString text = input_->text();
    input_->clear();

    if (!text.trimmed().isEmpty()) {
        history_.push_back(text);
    }
    historyCursor_ = history_.size();

    // The canvas takes what was typed first, when it is holding a grip or
    // showing dimensional input boxes. Nothing is running while a grip is held,
    // so without this the line would be executed as a command and "250" would
    // be reported as an unknown one.
    if (directInput_ && directInput_(text)) {
        refresh();
        Q_EMIT commandStateChanged();
        return;
    }

    CommandExecutor& executor = document_->executor();

    if (executor.isRunning()) {
        if (!supplyTyped(text)) {
            // Unparseable input is reported rather than silently ignored, and
            // the command keeps its place so the user can simply retype.
            executor.push(HistoryEntry::Kind::Error,
                          "Invalid input: " + text.toStdString());
        }
    } else if (text.trimmed().isEmpty()) {
        // Enter on an empty line repeats the last command, as other CAD software does.
        executor.repeatLast();
    } else {
        executor.execute(text.trimmed().toStdString());
    }

    document_->reindex();
    refresh();
    Q_EMIT commandStateChanged();


}


void CommandLine::clearInput() {
    input_->clear();
    Q_EMIT inputChanged();
}

void CommandLine::setDirectInput(std::function<bool(const QString&)> handler) {
    directInput_ = std::move(handler);
}

void CommandLine::setGripMode(std::function<QString()> mode) {
    gripMode_ = std::move(mode);
}

}  // namespace cad::app
