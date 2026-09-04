#pragma once

#include "cad/db/Color.hpp"
#include "cad/db/SymbolTable.hpp"

namespace cad {

inline constexpr const char* kDimStyleStandard = "Standard";

/// How a linear measurement is written.
///
/// Matches the DIMLUNIT system variable. Architectural and engineering formats
/// are not decoration: a drawing dimensioned in feet and inches must save and
/// reopen that way, and a value shown as `3'-6 1/2"` is not the same document
/// as one shown as `42.5`.
enum class LinearUnitsFormat : std::uint8_t {
    Scientific = 1,
    Decimal = 2,
    Engineering = 3,   ///< Feet and decimal inches.
    Architectural = 4, ///< Feet and fractional inches.
    Fractional = 5,
    WindowsDesktop = 6,
};

/// How an angular measurement is written. Matches DIMAUNIT.
enum class AngularUnitsFormat : std::uint8_t {
    DecimalDegrees = 0,
    DegreesMinutesSeconds = 1,
    Gradians = 2,
    Radians = 3,
    SurveyorUnits = 4,
};

/// Which zeros are left off. Matches the DIMZIN bit field.
///
/// Kept as a bit field rather than an enum because that is what it is, and
/// because the combinations are meaningful: suppressing leading zeros while
/// keeping trailing ones is an ordinary house style.
struct ZeroSuppression {
    /// Omit the feet part when it is zero, in feet-and-inches formats.
    bool suppressZeroFeet = true;
    /// Omit the inches part when it is zero.
    bool suppressZeroInches = true;
    /// `0.5` becomes `.5`.
    bool suppressLeading = false;
    /// `0.5000` becomes `0.5`.
    bool suppressTrailing = false;

    [[nodiscard]] static ZeroSuppression fromDimzin(int dimzin) noexcept;
    [[nodiscard]] int toDimzin() const noexcept;
};

/// Where dimension text sits relative to the dimension line. Matches DIMTAD.
enum class TextVerticalPosition : std::uint8_t {
    /// Centred on the dimension line, which is broken to make room.
    Centered = 0,
    /// Above the line — the ISO convention.
    Above = 1,
    /// On the far side from the first extension line.
    Outside = 2,
    /// JIS convention.
    Jis = 3,
    /// Below the line.
    Below = 4,
};

/// Horizontal placement of dimension text. Matches DIMJUST.
enum class TextHorizontalPosition : std::uint8_t {
    Centered = 0,
    NextToFirstExtension = 1,
    NextToSecondExtension = 2,
    AboveFirstExtension = 3,
    AboveSecondExtension = 4,
};

/// A named dimension style.
///
/// The largest symbol record in DWG by some distance — other CAD software exposes about
/// eighty DIM* system variables — because a dimension's appearance is almost
/// entirely a matter of house style, and two offices dimensioning the same
/// geometry produce visibly different drawings.
///
/// Modelled as data consumed at draw time rather than baked into the dimension
/// when it is created. That is what makes changing a style restyle every
/// dimension using it, which is the entire reason styles exist.
///
/// The subset here is the subset that changes what is drawn. Variables that do
/// not — alternate units and tolerances, for now — are carried through
/// unmodified by the base object's preserved group codes, so a drawing that
/// uses them is not damaged by being opened here.
class DimStyle final : public SymbolTableRecord {
public:
    // ------------------------------------------------------------- overall

    /// DIMSCALE. Multiplies every size below, so one style serves a drawing
    /// plotted at any scale. Zero means "scale to the viewport", which is
    /// resolved at draw time rather than stored.
    double overallScale = 1.0;

    // --------------------------------------------------------------- lines

    /// DIMCLRD. Colour of the dimension line and arrowheads.
    Color dimensionLineColor = Color::byBlock();
    /// DIMCLRE. Colour of the extension lines.
    Color extensionLineColor = Color::byBlock();
    /// DIMLWD / DIMLWE, in hundredths of a millimetre.
    std::int16_t dimensionLineWeight = toRaw(Lineweight::ByBlock);
    std::int16_t extensionLineWeight = toRaw(Lineweight::ByBlock);

    /// DIMDLE. How far the dimension line continues past the extension lines.
    double dimensionLineExtend = 0.0;
    /// DIMDLI. Spacing between the lines of a baseline dimension set.
    double baselineSpacing = 3.75;
    /// DIMSD1 / DIMSD2. Suppress the first or second half of the dimension
    /// line, measured from the text outwards.
    bool suppressDimensionLine1 = false;
    bool suppressDimensionLine2 = false;

    /// DIMEXE. How far extension lines continue past the dimension line.
    double extensionLineExtend = 1.25;
    /// DIMEXO. Gap between the measured geometry and the start of the
    /// extension line, so the dimension does not touch the part.
    double extensionLineOffset = 0.625;
    /// DIMSE1 / DIMSE2.
    bool suppressExtensionLine1 = false;
    bool suppressExtensionLine2 = false;
    /// DIMFXLON / DIMFXL. Extension lines of a fixed length rather than
    /// running back to the geometry.
    bool fixedExtensionLines = false;
    double fixedExtensionLength = 1.0;

    // ---------------------------------------------------------- arrowheads

    /// DIMASZ. Arrowhead length.
    double arrowSize = 2.5;
    /// DIMBLK / DIMBLK1 / DIMBLK2. Handles of block records used as custom
    /// arrowheads; null means the default closed filled arrow.
    Handle arrowBlock;
    Handle arrowBlock1;
    Handle arrowBlock2;
    /// DIMSAH. True when the two ends use different arrowhead blocks.
    bool separateArrowBlocks = false;
    /// DIMCEN. Centre mark size for radial dimensions. Positive draws a mark,
    /// negative draws centre *lines*, zero draws nothing — a sign convention
    /// inherited from other CAD software and preserved because files rely on it.
    double centerMarkSize = 2.5;

    // ---------------------------------------------------------------- text

    /// DIMTXSTY.
    Handle textStyleHandle;
    /// DIMTXT.
    double textHeight = 2.5;
    /// DIMCLRT.
    Color textColor = Color::byBlock();
    /// DIMGAP. Clearance between the text and the dimension line. Also the
    /// width of the box drawn around the text when it is negative.
    double textGap = 0.625;
    TextVerticalPosition textVerticalPosition = TextVerticalPosition::Above;
    TextHorizontalPosition textHorizontalPosition = TextHorizontalPosition::Centered;
    /// DIMTIH / DIMTOH. Force text horizontal when it is inside or outside the
    /// extension lines. Off in ISO practice, on in the imperial tradition.
    bool textInsideHorizontal = false;
    bool textOutsideHorizontal = false;
    /// DIMTFILL / DIMTFILLCLR. Opaque background behind the text.
    bool textHasBackground = false;
    Color textBackgroundColor = Color::fromIndex(0);

    // --------------------------------------------------------------- units

    /// DIMLFAC. Multiplies the measured distance before it is written. This is
    /// how a detail drawn at 1:5 is dimensioned in real-world units.
    double linearScale = 1.0;
    /// DIMRND. Round the measurement to this increment; zero disables.
    double roundOff = 0.0;
    LinearUnitsFormat linearFormat = LinearUnitsFormat::Decimal;
    /// DIMDEC. Decimal places, or the fraction denominator exponent in the
    /// fractional formats.
    int linearPrecision = 2;
    /// DIMDSEP. Usually '.'; ',' in much of Europe.
    char decimalSeparator = '.';
    ZeroSuppression linearZeroSuppression;
    /// DIMPOST. Prefix and suffix around the measurement, where `<>` marks
    /// where the measurement goes. Stored as written so it round-trips.
    std::string linearPostfix;

    AngularUnitsFormat angularFormat = AngularUnitsFormat::DecimalDegrees;
    /// DIMADEC.
    int angularPrecision = 0;
    ZeroSuppression angularZeroSuppression;

    [[nodiscard]] std::string objectType() const override { return "DimStyle"; }

    [[nodiscard]] bool isReserved() const override {
        return normalizeSymbolName(name) == normalizeSymbolName(kDimStyleStandard);
    }

    /// A size from this style, scaled by DIMSCALE.
    ///
    /// Every length above is a *paper* size; multiplying by the overall scale
    /// is what turns it into a model-space size. Forgetting the multiplication
    /// somewhere is the classic dimension-styling bug, so it lives here rather
    /// than at each of the twenty use sites.
    [[nodiscard]] double scaled(double size) const noexcept {
        return size * (overallScale > 0.0 ? overallScale : 1.0);
    }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

/// Apply the per-entity style overrides an object carries in its extended data.
///
/// A dimension may override any style variable for itself, and other CAD software stores
/// those in `ACAD` XData under a `DSTYLE` marker: a `1002 {` group, then pairs
/// of variable code and value, then `1002 }`. This is not an exotic corner —
/// nudging one dimension's arrow size or text height is routine, and a reader
/// that ignores overrides draws those dimensions visibly wrong while insisting
/// it read the file correctly.
///
/// The codes are the same ones the DIMSTYLE table record uses, so a variable
/// this application does not model is skipped here and still round-trips: the
/// XData itself is never modified.
///
/// @returns how many overrides were applied.
std::size_t applyDimStyleOverrides(DimStyle& style, const DbObject& object);

/// Whether `object` carries any DSTYLE override at all.
///
/// Separate from applying them so the common case — nearly every dimension —
/// costs a map lookup rather than a copy of the style.
[[nodiscard]] bool hasDimStyleOverrides(const DbObject& object);

/// Write a measured length the way this style says to.
///
/// Includes the DIMLFAC scaling, DIMRND rounding, the unit format, zero
/// suppression and the DIMPOST prefix/suffix.
[[nodiscard]] std::string formatLinearMeasurement(const DimStyle& style, double distance);

/// Write a measured angle, in radians, the way this style says to.
[[nodiscard]] std::string formatAngularMeasurement(const DimStyle& style, double radians);

}  // namespace cad
