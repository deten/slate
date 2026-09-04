#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cad/db/Color.hpp"

namespace cad {

/// One stretch of MText drawn with uniform formatting.
///
/// MText is not a string with a font — it is a string with formatting *inside*
/// it, so a note can say "GENERAL NOTES" in bold above a body in normal weight,
/// with one word in red because it changed. Reducing it to plain text, which is
/// what this application did, throws every one of those distinctions away and
/// draws the whole note in one size and one colour.
///
/// Everything here is expressed relative to the entity, not absolutely: the
/// height is a *factor* on the MText's own height, and an unset colour means the
/// entity's. That is what lets a run survive the entity being restyled, and it
/// is how the format itself stores the common cases.
struct MTextRun {
    std::string text;

    /// Multiplier on the entity's height. `\H2x;` gives 2.0.
    double heightFactor = 1.0;
    /// Multiplier on the style's width factor, from `\W`.
    double widthFactor = 1.0;
    /// Extra obliquing in radians, from `\Q`.
    double oblique = 0.0;

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool overline = false;
    bool strikethrough = false;

    /// Unset means the entity's own colour.
    std::optional<Color> color;
    /// Empty means the entity's text style.
    std::string font;

    /// This run is the last on its line: a `\P` followed it.
    bool endsParagraph = false;

    /// Formatting codes that appeared before this run's text and that this
    /// parser does not model, kept exactly as written.
    ///
    /// Without them, editing a note through a rich editor would silently drop
    /// every code the editor cannot show — paragraph indents, tracking,
    /// alignment — because the round trip goes text → runs → text. Dropping
    /// what you cannot display is the failure this whole codebase is careful
    /// about, and a text editor is no more entitled to it than a file writer.
    std::string unmodelledCodes;

    [[nodiscard]] bool sameFormatting(const MTextRun& other) const noexcept;
};

/// Split MText contents into formatted runs.
///
/// Braces open and close a formatting scope, so `{\C1;red} black` leaves the
/// second word alone — that nesting is the whole reason a stack is needed rather
/// than a running set of flags.
///
/// Codes this does not understand are *dropped rather than shown*, which is the
/// same choice `plainText` makes and for the same reason: a code rendered as its
/// own characters is worse than a code not rendered at all. What it must never
/// do is drop the text around one, which is the bug that hid `\L` underlined
/// runs entirely.
[[nodiscard]] std::vector<MTextRun> parseMText(std::string_view contents);

/// The plain text of a run list, paragraph breaks as newlines.
[[nodiscard]] std::string runsToPlainText(const std::vector<MTextRun>& runs);

/// Serialise runs back into MText contents: the inverse of `parseMText`.
///
/// Each formatted run is wrapped in its own brace scope, which makes the
/// nesting trivially correct — a run cannot leak its formatting into the next
/// one, which is the failure mode of emitting codes without scopes.
///
/// @param defaultFont the text style's own typeface, used when a run is bold or
///   italic without naming a font. MText has no standalone bold code: weight is
///   part of a font selection, so making text bold requires a family to make it
///   bold *of*. With no family available the weight cannot be expressed and is
///   dropped rather than written as something that will not render.
[[nodiscard]] std::string formatRuns(const std::vector<MTextRun>& runs,
                                     std::string_view defaultFont = {});

}  // namespace cad
