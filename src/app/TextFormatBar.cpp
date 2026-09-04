#include "TextFormatBar.hpp"

#include <algorithm>

#include <QColorDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QToolButton>

#include "Theme.hpp"

namespace cad::app {
namespace {

constexpr int kButtonSize = 24;

[[nodiscard]] QToolButton* makeButton(QWidget* parent, const QString& label,
                                      const QString& tooltip, bool checkable) {
    auto* button = new QToolButton{parent};
    button->setText(label);
    button->setToolTip(tooltip);
    button->setCheckable(checkable);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    // The whole point: pressing Bold must not take the keyboard, or the
    // selection it is about to format is gone before it is formatted.
    button->setFocusPolicy(Qt::NoFocus);
    // Square by default, but never narrower than the button says it needs to
    // be. A fixed 24px hid "A+" and "A-" completely: QToolButton elides text it
    // cannot fit, and a two-character label plus the style's own margins does
    // not fit in 24, so both rendered as blank gaps in the middle of the bar.
    // Asking the button rather than measuring the string, because the margins
    // that overflowed it belong to the style and are not mine to guess at.
    button->setFixedSize(std::max(kButtonSize, button->sizeHint().width()), kButtonSize);
    return button;
}

}  // namespace

TextFormatBar::TextFormatBar(QWidget* parent) : QWidget{parent} {
    setFocusPolicy(Qt::NoFocus);

    auto* row = new QHBoxLayout{this};
    row->setContentsMargins(4, 3, 4, 3);
    row->setSpacing(2);

    bold_ = makeButton(this, tr("B"), tr("Bold (Ctrl+B)"), true);
    italic_ = makeButton(this, tr("I"), tr("Italic (Ctrl+I)"), true);
    underline_ = makeButton(this, tr("U"), tr("Underline (Ctrl+U)"), true);

    // Named so the style sheet can letter them the way they act. Setting a
    // QFont on each instead does not survive: applying a style sheet re-polishes
    // the widget and takes the font back with it, so the B stopped being bold
    // the moment the sheet at the end of this constructor was applied.
    bold_->setObjectName("bold");
    italic_->setObjectName("italic");
    underline_->setObjectName("underline");

    auto* bigger = makeButton(this, tr("A+"), tr("Make the selected text larger"), false);
    auto* smaller = makeButton(this, tr("A-"), tr("Make the selected text smaller"), false);
    auto* color = makeButton(this, tr("A"), tr("Colour the selected text"), false);
    auto* clear = makeButton(this, tr("✕"), tr("Remove formatting from the selection"), false);

    const auto separator = [this] {
        auto* line = new QFrame{this};
        line->setFrameShape(QFrame::VLine);
        line->setFixedWidth(1);
        line->setStyleSheet(QString{"background: %1; border: none;"}
                                .arg(currentTheme().panelBorder.name()));
        return line;
    };

    row->addWidget(bold_);
    row->addWidget(italic_);
    row->addWidget(underline_);
    row->addWidget(separator());
    row->addWidget(bigger);
    row->addWidget(smaller);
    row->addWidget(separator());
    row->addWidget(color);
    row->addWidget(clear);

    connect(bold_, &QToolButton::toggled, this, [this](bool on) {
        if (!syncing_) {
            Q_EMIT boldToggled(on);
        }
    });
    connect(italic_, &QToolButton::toggled, this, [this](bool on) {
        if (!syncing_) {
            Q_EMIT italicToggled(on);
        }
    });
    connect(underline_, &QToolButton::toggled, this, [this](bool on) {
        if (!syncing_) {
            Q_EMIT underlineToggled(on);
        }
    });
    // A fixed step rather than a size box: MText heights are a factor on the
    // entity's, and a drafter nudging a heading wants it bigger, not 1.15.
    connect(bigger, &QToolButton::clicked, this, [this] { Q_EMIT heightScaled(1.25); });
    connect(smaller, &QToolButton::clicked, this, [this] { Q_EMIT heightScaled(1.0 / 1.25); });
    connect(clear, &QToolButton::clicked, this, [this] { Q_EMIT formattingCleared(); });
    connect(color, &QToolButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(Qt::white, this, tr("Text colour"));
        if (chosen.isValid()) {
            Q_EMIT colorPicked(chosen);
        }
    });

    const Theme& theme = currentTheme();
    setStyleSheet(QString{R"(
        cad--app--TextFormatBar {
            background: %1;
            border: 1px solid %2;
            border-radius: 5px;
        }
        QToolButton { color: %3; border: none; border-radius: 3px; }
        QToolButton:hover { background: %4; }
        QToolButton:checked { background: %2; color: white; }
        QToolButton#bold { font-weight: bold; }
        QToolButton#italic { font-style: italic; }
        QToolButton#underline { text-decoration: underline; }
    )"}
                      .arg(theme.panel.name(), theme.accent.name(), theme.text.name(),
                           theme.panelAlternate.name()));
    // Without this a plain QWidget ignores `background` and `border` from a
    // style sheet entirely, so the bar was a row of glyphs floating on the
    // drawing with nothing behind them.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);
    adjustSize();
}

void TextFormatBar::showState(bool bold, bool italic, bool underline) {
    // Guarded, because setChecked emits `toggled` and would otherwise read as
    // the user pressing every button that happened to change.
    syncing_ = true;
    bold_->setChecked(bold);
    italic_->setChecked(italic);
    underline_->setChecked(underline);
    syncing_ = false;
}

}  // namespace cad::app
