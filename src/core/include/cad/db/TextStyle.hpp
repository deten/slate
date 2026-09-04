#pragma once

#include "cad/db/SymbolTable.hpp"

namespace cad {

inline constexpr const char* kTextStyleStandard = "Standard";

/// A text style: the named font-and-formatting record that TEXT, MTEXT,
/// dimensions and attributes reference.
///
/// DWG text styles may name either an SHX shape font (other CAD software's own vector
/// format) or a TrueType font. Both are recorded; which one the renderer can
/// actually draw is a separate concern handled by font resolution, and a style
/// naming a font we cannot load still keeps its metrics so the layout stays
/// correct.
class TextStyle final : public SymbolTableRecord {
public:
    /// Primary font file, e.g. `txt.shx` or `arial.ttf`. Kept as written in the
    /// file so resource resolution can search for it.
    std::string fontFile = "txt.shx";

    /// Secondary font used for Asian glyphs ("big font").
    std::string bigFontFile;

    /// TrueType family name, when the style names a system font.
    std::string typeface;

    /// Fixed text height. Zero means the height is supplied per text object,
    /// which is the common case and why it cannot be defaulted to non-zero.
    double height = 0.0;

    /// Width scale factor applied to each glyph.
    double widthFactor = 1.0;

    /// Slant angle in radians; positive leans right.
    double obliqueAngle = 0.0;

    /// Text is drawn mirrored across its baseline.
    bool isBackwards = false;

    /// Text is drawn upside down.
    bool isUpsideDown = false;

    /// Glyphs are rotated 90 degrees and stacked vertically.
    bool isVertical = false;

    /// TrueType weight, 400 for regular and 700 for bold.
    int fontWeight = 400;

    /// True when the TrueType face is italic.
    bool isItalic = false;

    /// True for styles that exist only to hold shape-file definitions and are
    /// not offered in the style picker.
    bool isShapeFile = false;

    [[nodiscard]] std::string objectType() const override { return "TextStyle"; }

    [[nodiscard]] bool isReserved() const override {
        return normalizeSymbolName(name) == normalizeSymbolName(kTextStyleStandard);
    }

    /// True when text objects must supply their own height.
    [[nodiscard]] bool isHeightVariable() const noexcept { return height == 0.0; }

    /// True when the style names a TrueType rather than an SHX font.
    [[nodiscard]] bool isTrueType() const;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
