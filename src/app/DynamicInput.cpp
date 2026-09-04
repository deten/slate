#include "DynamicInput.hpp"

#include <algorithm>
#include <QHBoxLayout>
#include <QLayout>
#include <QMargins>
#include <QLabel>

#include "Theme.hpp"

namespace cad::app {
namespace {

/// Offset from the crosshair. Far enough not to sit under the cursor, close
/// enough to read without moving the eye.
constexpr int kOffsetX = 18;
constexpr int kOffsetY = 14;

}  // namespace

DynamicInput::DynamicInput(QWidget* parent) : QWidget{parent} {
    // Transparent to the mouse: the canvas underneath must still receive
    // clicks and moves, or the prompt would block the very geometry it is
    // there to help you point at.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    hide();

    auto* layout = new QHBoxLayout{this};
    layout->setContentsMargins(9, 5, 9, 5);
    layout->setSpacing(8);

    const Theme& theme = currentTheme();
    prompt_ = new QLabel{this};
    prompt_->setStyleSheet(QString{"color: %1;"}.arg(theme.textMuted.name()));
    text_ = new QLabel{this};
    text_->setStyleSheet(
        QString{"color: %1; font-weight: 600;"}.arg(theme.foreground.name()));

    layout->addWidget(prompt_);
    layout->addWidget(text_);

    setStyleSheet(QString{R"(
        cad--app--DynamicInput {
            background: %1;
            border: 1px solid %2;
            border-radius: 6px;
        }
    )"}
                      .arg(theme.panel.name(), theme.accent.name()));
    setAutoFillBackground(true);
}

void DynamicInput::showAt(const QPoint& cursor, const QString& prompt, const QString& text) {
    // A caret, so an empty buffer still reads as "waiting for you" rather than
    // as an empty box.
    place(cursor, prompt, text + QStringLiteral("_"));
}

void DynamicInput::place(const QPoint& cursor, const QString& prompt, const QString& text) {
    prompt_->setText(prompt);
    prompt_->setVisible(!prompt.isEmpty());
    text_->setText(text);

    // Sized from the strings, not from the layout.
    //
    // Asking the widget how wide it is returns the width it had for the
    // *previous* text, because Qt lays a widget out on the next event loop turn
    // and the position is wanted now. Showing it first to force the layout does
    // not help either: the box has to be placed in the same breath it is shown,
    // or it appears in the wrong place and jumps. The text is the one thing
    // whose width can be known here, so it is what the box is sized from --
    // and the box exists to hold exactly that text.
    constexpr int kSideMargin = 9;
    constexpr int kTopMargin = 5;
    constexpr int kSpacing = 8;

    int wanted = kSideMargin * 2;
    if (!prompt.isEmpty()) {
        wanted += prompt_->fontMetrics().horizontalAdvance(prompt) + kSpacing;
    }
    wanted += text_->fontMetrics().horizontalAdvance(text);
    const int tall = std::max(prompt_->fontMetrics().height(), text_->fontMetrics().height()) +
                     kTopMargin * 2;
    // Fixed rather than merely resized: a plain `resize` is renegotiated by the
    // layout the moment the widget is shown, which is the whole problem.
    setFixedSize(wanted + 2, tall + 2);

    QPoint at = cursor + QPoint{kOffsetX, kOffsetY};
    if (parentWidget() != nullptr) {
        const QRect bounds = parentWidget()->rect();
        // Flip to the other side of the cursor rather than clamping when it
        // would overhang: clamping leaves the prompt sitting on top of the
        // crosshair at the edge of the window.
        if (at.x() + width() > bounds.right()) {
            at.setX(cursor.x() - kOffsetX - width());
        }
        if (at.y() + height() > bounds.bottom()) {
            at.setY(cursor.y() - kOffsetY - height());
        }
        at.setX(std::max(bounds.left(), at.x()));
        at.setY(std::max(bounds.top(), at.y()));
    }
    move(at);
    show();
    raise();
}

void DynamicInput::hideInput() {
    hide();
}

}  // namespace cad::app
