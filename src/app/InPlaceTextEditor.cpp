#include "InPlaceTextEditor.hpp"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

#include "TextFormatBar.hpp"
#include "Theme.hpp"

namespace cad::app {
namespace {

/// Smallest readable editor, in device pixels.
///
/// Text zoomed out to three pixels is drawn as a bar; it cannot be *typed* into
/// at three pixels. Below this the editor uses a legible size and stops
/// pretending to be what it is editing — the alternative is a working editor
/// nobody can see.
constexpr double kMinimumPixelHeight = 11.0;

constexpr int kPaddingX = 4;
constexpr int kPaddingY = 2;

/// Custom character-format properties.
///
/// `kHeightFactor` is MText's own idea of size — a multiple of the entity's
/// height — which has to survive a trip through a document whose sizes are in
/// points. `kUnmodelled` carries the codes this application cannot show, so that
/// editing a note does not delete its paragraph indents. `kExplicitColor` marks
/// a colour the *user* chose, because Qt fills the foreground in for every
/// fragment and reading it unconditionally would write a colour code onto every
/// run in the note.
constexpr int kHeightFactor = QTextFormat::UserProperty;
constexpr int kUnmodelled = QTextFormat::UserProperty + 1;
constexpr int kExplicitColor = QTextFormat::UserProperty + 2;
/// The family the run *named*, which is not always one the system can draw
/// with — see `fontIsAvailable` in ScenePainter for what happens when it is not.
constexpr int kFontName = QTextFormat::UserProperty + 3;

}  // namespace

InPlaceTextEditor::InPlaceTextEditor(QWidget* parent) : QTextEdit{parent} {
    setFrameStyle(QFrame::NoFrame);
    setLineWrapMode(QTextEdit::NoWrap);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setContextMenuPolicy(Qt::NoContextMenu);
    setAcceptRichText(false);
    hide();

    bar_ = new TextFormatBar{parent};
    bar_->hide();

    connect(bar_, &TextFormatBar::boldToggled, this, [this](bool on) {
        QTextCharFormat format;
        format.setFontWeight(on ? QFont::Bold : QFont::Normal);
        applyToSelection(format);
    });
    connect(bar_, &TextFormatBar::italicToggled, this, [this](bool on) {
        QTextCharFormat format;
        format.setFontItalic(on);
        applyToSelection(format);
    });
    connect(bar_, &TextFormatBar::underlineToggled, this, [this](bool on) {
        QTextCharFormat format;
        format.setFontUnderline(on);
        applyToSelection(format);
    });
    connect(bar_, &TextFormatBar::colorPicked, this, [this](const QColor& color) {
        QTextCharFormat format;
        format.setForeground(color);
        format.setProperty(kExplicitColor, true);
        applyToSelection(format);
    });

    // Height is applied per character rather than per selection, because the
    // factor being scaled is whatever each character already had: making a
    // mixed selection larger has to keep its relative sizes.
    connect(bar_, &TextFormatBar::heightScaled, this, [this](double factor) {
        QTextCursor cursor = textCursor();
        if (!cursor.hasSelection()) {
            return;
        }
        const int from = cursor.selectionStart();
        const int to = cursor.selectionEnd();
        for (int at = from; at < to; ++at) {
            QTextCursor one{document()};
            one.setPosition(at);
            one.setPosition(at + 1, QTextCursor::KeepAnchor);
            QTextCharFormat format = one.charFormat();
            const double current =
                format.hasProperty(kHeightFactor) ? format.doubleProperty(kHeightFactor) : 1.0;
            const double scaled = std::clamp(current * factor, 0.1, 10.0);
            format.setProperty(kHeightFactor, scaled);
            format.setFontPointSize(std::max(1.0, pixelHeight_ * scaled));
            one.mergeCharFormat(format);
        }
        cursor.setPosition(from);
        cursor.setPosition(to, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
        setFocus(Qt::OtherFocusReason);
    });

    connect(bar_, &TextFormatBar::formattingCleared, this, [this] {
        QTextCursor cursor = textCursor();
        if (!cursor.hasSelection()) {
            return;
        }
        QTextCharFormat plain;
        plain.setFontWeight(QFont::Normal);
        plain.setFontItalic(false);
        plain.setFontUnderline(false);
        plain.setFontOverline(false);
        plain.setFontStrikeOut(false);
        plain.setFontPointSize(std::max(1.0, pixelHeight_));
        plain.setProperty(kHeightFactor, 1.0);
        plain.setProperty(kExplicitColor, false);
        plain.setForeground(palette().text());
        cursor.setCharFormat(plain);
        setFocus(Qt::OtherFocusReason);
    });

    // The bar says what is true of the text at the cursor, not what was last
    // pressed — otherwise it is a set of stale switches.
    connect(this, &QTextEdit::currentCharFormatChanged, this,
            [this](const QTextCharFormat& format) {
                if (bar_ != nullptr) {
                    bar_->showState(format.fontWeight() > QFont::Normal, format.fontItalic(),
                                    format.fontUnderline());
                }
            });
}

void InPlaceTextEditor::applyToSelection(const QTextCharFormat& format) {
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        cursor.mergeCharFormat(format);
    } else {
        // No selection means "from here on", which is what every editor does
        // and what makes typing a bold word possible.
        mergeCurrentCharFormat(format);
    }
    // The bar does not take focus, but pressing it moves the active window's
    // idea of what is being typed into; putting it back keeps the caret alive.
    setFocus(Qt::OtherFocusReason);
}

bool InPlaceTextEditor::ownsWidget(const QWidget* widget) const {
    for (const QWidget* at = widget; at != nullptr; at = at->parentWidget()) {
        if (at == this || at == bar_) {
            return true;
        }
    }
    return false;
}

void InPlaceTextEditor::setRuns(const std::vector<MTextRun>& runs) {
    QTextDocument* target = document();
    target->clear();

    QTextCursor cursor{target};
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const MTextRun& run = runs[i];

        QTextCharFormat format;
        format.setFontWeight(run.bold ? QFont::Bold : QFont::Normal);
        format.setFontItalic(run.italic);
        format.setFontUnderline(run.underline);
        format.setFontOverline(run.overline);
        format.setFontStrikeOut(run.strikethrough);
        format.setFontPointSize(std::max(1.0, pixelHeight_ * run.heightFactor));
        format.setProperty(kHeightFactor, run.heightFactor);
        // The family is recorded on the format so it survives the round trip,
        // but only applied to the *display* when the system has it: an SHX
        // shape file is not a font, and Qt substitutes a symbol face for it.
        if (!run.font.empty()) {
            const QString family = QString::fromStdString(run.font);
            format.setProperty(kFontName, family);
            if (!family.endsWith(".shx", Qt::CaseInsensitive) &&
                QFontDatabase::families().contains(family, Qt::CaseInsensitive)) {
                format.setFontFamilies({family});
            }
        }
        if (run.color) {
            format.setForeground(QColor::fromRgb(static_cast<QRgb>(run.color->toRgb())));
            format.setProperty(kExplicitColor, true);
        }
        if (!run.unmodelledCodes.empty()) {
            format.setProperty(kUnmodelled, QString::fromStdString(run.unmodelledCodes));
        }

        cursor.insertText(QString::fromStdString(run.text), format);
        if (run.endsParagraph && i + 1 < runs.size()) {
            cursor.insertBlock();
        }
    }
}

std::vector<MTextRun> InPlaceTextEditor::currentRuns() const {
    std::vector<MTextRun> runs;
    const QTextDocument* target = document();

    for (QTextBlock block = target->begin(); block.isValid(); block = block.next()) {
        bool any = false;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.text().isEmpty()) {
                continue;
            }
            const QTextCharFormat format = fragment.charFormat();

            MTextRun run;
            run.text = fragment.text().toStdString();
            run.bold = format.fontWeight() > QFont::Normal;
            run.italic = format.fontItalic();
            run.underline = format.fontUnderline();
            run.overline = format.fontOverline();
            run.strikethrough = format.fontStrikeOut();
            run.heightFactor =
                format.hasProperty(kHeightFactor) ? format.doubleProperty(kHeightFactor) : 1.0;

            // The name it came in with, not the one Qt is drawing it with.
            if (format.hasProperty(kFontName)) {
                const QString family = format.stringProperty(kFontName);
                if (family != defaultFont_) {
                    run.font = family.toStdString();
                }
            }
            if (format.boolProperty(kExplicitColor)) {
                run.color = Color::fromTrueColor(
                    static_cast<std::uint32_t>(format.foreground().color().rgb() & 0xFFFFFFu));
            }
            if (format.hasProperty(kUnmodelled)) {
                run.unmodelledCodes = format.stringProperty(kUnmodelled).toStdString();
            }
            runs.push_back(std::move(run));
            any = true;
        }
        if (!any) {
            // An empty line is still a line.
            runs.push_back(MTextRun{});
        }
        if (block.next().isValid()) {
            runs.back().endsParagraph = true;
        }
    }
    return runs;
}

void InPlaceTextEditor::beginEdit(const Placement& placement, const QString& contents) {
    multiLine_ = placement.multiLine;
    pixelHeight_ = std::max(placement.pixelHeight, kMinimumPixelHeight);
    defaultFont_ = placement.defaultFont;
    original_ = contents;

    const Theme& theme = currentTheme();
    // The size goes in the style sheet rather than through `setFont`, because
    // applying a style sheet re-polishes the widget and puts the inherited font
    // back — the editor came out at the UI's 10pt over text drawn at 67 pixels,
    // which is the opposite of editing something at the size it will be.
    setStyleSheet(QString{R"(
        QTextEdit {
            background: %1;
            color: %2;
            border: 1px solid %3;
            border-radius: 3px;
            padding: %4px %5px;
            font-size: %6px;
            selection-background-color: %3;
        }
    )"}
                      .arg(theme.panel.name(), placement.color.name(), theme.accent.name())
                      .arg(kPaddingY)
                      .arg(kPaddingX)
                      .arg(static_cast<int>(std::lround(pixelHeight_))));

    setRuns(parseMText(contents.toStdString()));
    selectAll();

    // Sized to the entity *and* to what it now holds, whichever is larger. The
    // entity's box alone is not enough: an editor's line spacing is not the
    // drawing's, so a two-line note came out with its second line clipped by
    // the box the note itself fits in.
    QFont measured = font();
    measured.setPixelSize(static_cast<int>(std::lround(pixelHeight_)));
    const QFontMetricsF metrics{measured};
    const QStringList lines = toPlainText().split('\n');

    double contentWidth = 0.0;
    for (const QString& line : lines) {
        contentWidth = std::max(contentWidth, metrics.horizontalAdvance(line));
    }
    // Room for a run that has been made larger than the rest, and a line spare
    // so pressing Enter at the end does not push the caret out of sight.
    contentWidth *= 1.35;
    const double contentHeight =
        metrics.lineSpacing() * static_cast<double>(lines.size() + (multiLine_ ? 1 : 0)) * 1.25;

    const int minimumWidth = static_cast<int>(std::lround(pixelHeight_ * 6.0));
    const int minimumHeight = static_cast<int>(std::lround(pixelHeight_ * 1.6));
    const int boxWidth = std::max({minimumWidth,
                                   static_cast<int>(std::lround(placement.rect.width())),
                                   static_cast<int>(std::ceil(contentWidth))}) +
                         kPaddingX * 2;
    const int boxHeight =
        std::max({minimumHeight, static_cast<int>(std::ceil(contentHeight))}) + kPaddingY * 2;

    setGeometry(QRect{static_cast<int>(std::lround(placement.rect.left())) - kPaddingX,
                      static_cast<int>(std::lround(placement.rect.top())) - kPaddingY, boxWidth,
                      boxHeight});

    editing_ = true;
    finishing_ = false;
    show();
    raise();
    positionBar();
    setFocus(Qt::OtherFocusReason);
}

void InPlaceTextEditor::positionBar() {
    if (bar_ == nullptr || parentWidget() == nullptr) {
        return;
    }
    bar_->adjustSize();
    // Above the editor, and pushed back inside the canvas when the note is near
    // an edge — a bar half off the window is worse than one a few pixels from
    // where it was asked to go.
    const int x =
        std::clamp(geometry().left(), 0, std::max(0, parentWidget()->width() - bar_->width()));
    int y = geometry().top() - bar_->height() - 6;
    if (y < 0) {
        y = geometry().bottom() + 6;
    }
    bar_->move(x, y);
    bar_->show();
    bar_->raise();
}

void InPlaceTextEditor::abandon() {
    editing_ = false;
    finishing_ = true;
    hide();
    if (bar_ != nullptr) {
        bar_->hide();
    }
    finishing_ = false;
}

/// Deliberately not committed on losing focus.
///
/// It reads as the obvious rule and it is a trap: focus moves for reasons that
/// have nothing to do with the user finishing — another widget refreshing, the
/// window deactivating, a tooltip, or the formatting bar being pressed. Each
/// would end the edit silently with whatever happened to be in the box. The
/// canvas commits on a click somewhere else, which is the event that actually
/// means "done".
void InPlaceTextEditor::commit() {
    if (!editing_ || finishing_) {
        return;
    }
    finishing_ = true;
    editing_ = false;

    const std::vector<MTextRun> runs = currentRuns();
    QString contents = QString::fromStdString(formatRuns(runs, defaultFont_.toStdString()));

    // A note that comes back saying the same thing in the same way is written
    // back *as it arrived*. Re-serialising an untouched note would rewrite its
    // codes into this application's own spelling of them — a diff on a drawing
    // nobody edited, and a chance to lose something for no gain at all.
    const std::vector<MTextRun> before = parseMText(original_.toStdString());
    if (before.size() == runs.size()) {
        bool identical = true;
        for (std::size_t i = 0; i < runs.size() && identical; ++i) {
            identical = runs[i].text == before[i].text &&
                        runs[i].endsParagraph == before[i].endsParagraph &&
                        runs[i].sameFormatting(before[i]);
        }
        if (identical) {
            contents = original_;
        }
    }

    hide();
    if (bar_ != nullptr) {
        bar_->hide();
    }
    finishing_ = false;
    Q_EMIT committed(contents);
}

void InPlaceTextEditor::cancel() {
    if (!editing_ || finishing_) {
        return;
    }
    finishing_ = true;
    editing_ = false;
    hide();
    if (bar_ != nullptr) {
        bar_->hide();
    }
    finishing_ = false;
    Q_EMIT cancelled();
}

void InPlaceTextEditor::commitNow() {
    commit();
}

void InPlaceTextEditor::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        cancel();
        return;
    }

    // The shortcuts every text editor has. Without them a drafter has to reach
    // for the mouse to bold a word, which is most of the reason they would not
    // bother.
    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        QTextCharFormat format;
        bool handled = true;
        switch (event->key()) {
            case Qt::Key_B:
                format.setFontWeight(fontWeight() > QFont::Normal ? QFont::Normal : QFont::Bold);
                break;
            case Qt::Key_I:
                format.setFontItalic(!fontItalic());
                break;
            case Qt::Key_U:
                format.setFontUnderline(!fontUnderline());
                break;
            default:
                handled = false;
                break;
        }
        if (handled) {
            applyToSelection(format);
            event->accept();
            return;
        }
    }

    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (enter) {
        // A single line commits on Enter, because that is what a single line
        // means. A paragraph commits on Ctrl+Enter and takes Enter as a line
        // break, which is other CAD software's rule and the only one that lets a note be
        // written at all.
        const bool withControl = (event->modifiers() & Qt::ControlModifier) != 0;
        if (!multiLine_ || withControl) {
            commit();
            return;
        }
    }

    QTextEdit::keyPressEvent(event);
}

}  // namespace cad::app
