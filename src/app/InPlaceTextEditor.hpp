#pragma once

#include <QColor>
#include <QRectF>
#include <QString>
#include <QTextCharFormat>
#include <QTextEdit>
#include <vector>

#include "cad/db/MTextFormat.hpp"

namespace cad::app {

class TextFormatBar;

/// Editing a note where it sits, at the size it will be, with its formatting.
///
/// The command line can already change a label's words, and for one word that is
/// enough. For a paragraph it is not: a note is composed *against* the drawing —
/// it has to fit between two walls, not collide with a dimension, and wrap at a
/// column the drafter chose. None of that is visible from a one-line box at the
/// bottom of the window.
///
/// It is a **rich** editor rather than one showing MText's codes, and that is a
/// decision with a cost worth stating. Showing the codes is honest and useless:
/// nobody composes a note reading `{\fArial|b1|i0|c0|p0;GENERAL NOTES}\P`. So the
/// codes are parsed into runs, the runs become character formatting, and the
/// formatting becomes codes again on the way out.
///
/// The risk of that round trip is silent loss, and it is handled rather than
/// hoped about. Codes this application does not model are carried through as an
/// opaque property on the run that followed them; and a note whose text and
/// formatting come back unchanged is written back as **the original string**,
/// byte for byte, rather than as a re-serialisation of itself.
class InPlaceTextEditor final : public QTextEdit {
    Q_OBJECT

public:
    /// Where and how to draw the editor, in the parent's device pixels.
    struct Placement {
        /// The box the text occupies on screen.
        QRectF rect;
        /// Text height in device pixels, so the editor reads at the size the
        /// drawing will.
        double pixelHeight = 12.0;
        QColor color;
        /// Enter inserts a line break rather than committing.
        bool multiLine = false;
        /// The text style's own typeface. Needed because MText has no
        /// standalone bold code — weight is part of a font selection, so making
        /// text bold requires a family to make it bold *of*.
        QString defaultFont;
    };

    explicit InPlaceTextEditor(QWidget* parent = nullptr);

    /// Show the editor over `placement`, holding `contents` in MText's own coded
    /// form, all of it selected.
    void beginEdit(const Placement& placement, const QString& contents);

    /// Accept what is typed, as if the user had pressed Enter. For a click
    /// somewhere else on the drawing, which is how an in-place editor is
    /// usually finished.
    void commitNow();

    /// Hide without emitting anything. For the case where the command ended on
    /// its own — an Escape typed at the command line, an undo, a new drawing.
    void abandon();

    [[nodiscard]] bool isEditing() const noexcept { return editing_; }

    /// True when `widget` is the editor or part of its furniture.
    ///
    /// A click on the formatting bar is not a click *away* from the editor, and
    /// treating it as one would commit the note the instant somebody reached
    /// for Bold.
    [[nodiscard]] bool ownsWidget(const QWidget* widget) const;

Q_SIGNALS:
    /// The user accepted the text. The string is MText contents, coded.
    void committed(const QString& contents);

    /// The user pressed Escape.
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void commit();
    void cancel();

    /// Merge a character format into the selection, or into what is typed next
    /// when there is none.
    void applyToSelection(const QTextCharFormat& format);

    /// Build the document from parsed runs.
    void setRuns(const std::vector<MTextRun>& runs);

    /// Read the document back out as runs.
    [[nodiscard]] std::vector<MTextRun> currentRuns() const;

    void positionBar();

    bool editing_ = false;
    bool multiLine_ = false;
    /// Guards against re-entering `commit` from the change it causes.
    bool finishing_ = false;
    double pixelHeight_ = 12.0;
    QString defaultFont_;
    /// What arrived, so an unchanged note is written back untouched.
    QString original_;

    TextFormatBar* bar_ = nullptr;
};

}  // namespace cad::app
