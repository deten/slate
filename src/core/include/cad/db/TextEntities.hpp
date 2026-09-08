#pragma once

#include <functional>
#include <string>

#include "cad/db/Entity.hpp"
#include "cad/db/MTextFormat.hpp"

namespace cad {

/// Horizontal justification of a single-line TEXT entity.
enum class TextHorizontalAlignment : std::uint8_t {
    Left = 0,
    Center = 1,
    Right = 2,
    Aligned = 3,
    Middle = 4,
    Fit = 5,
};

/// Vertical justification of a single-line TEXT entity.
enum class TextVerticalAlignment : std::uint8_t {
    Baseline = 0,
    Bottom = 1,
    Middle = 2,
    Top = 3,
};

/// Estimated text advance width, used for bounds.
///
/// Exact bounds require measuring the font, which the core has no access to —
/// it must stay platform-free. The renderer installs a real measurement
/// function via `setTextMeasurer`; until then a proportional estimate keeps
/// text selectable and roughly correct in headless contexts.
using TextMeasurer = std::function<double(std::string_view text, double height,
                                          double widthFactor)>;

/// Install a real font-metrics function. Called once by the rendering layer at
/// start-up so entity bounds — and therefore selection and zoom extents —
/// match what is actually drawn.
void setTextMeasurer(TextMeasurer measurer);

[[nodiscard]] double measureText(std::string_view text, double height, double widthFactor);

/// Text as it reads on the drawing: the `%%d`-style symbol codes files carry
/// (degree, diameter, plus-minus, a literal percent) turned into their
/// characters, and the underline and overline toggles dropped. The stored
/// text keeps the codes as written, so a file gets them back unchanged.
[[nodiscard]] std::string decodeTextSymbols(std::string_view text);

/// A single-line text entity — TEXT.
class TextEntity : public Entity {
public:
    /// Insertion point. Its meaning depends on the alignment settings.
    Vec3 position;

    /// Secondary alignment point, used when the text is centred, aligned or
    /// fit. DWG stores both, and which is authoritative depends on alignment.
    std::optional<Vec3> alignmentPoint;

    std::string contents;
    double height = 2.5;
    /// Rotation about the normal, in radians.
    double rotation = 0.0;
    double widthFactor = 1.0;
    double obliqueAngle = 0.0;
    Handle styleHandle;
    TextHorizontalAlignment horizontalAlignment = TextHorizontalAlignment::Left;
    TextVerticalAlignment verticalAlignment = TextVerticalAlignment::Baseline;
    bool isBackwards = false;
    bool isUpsideDown = false;
    double thickness = 0.0;

    TextEntity() = default;
    TextEntity(const Vec3& where, std::string what, double textHeight)
        : position(where), contents(std::move(what)), height(textHeight) {}

    [[nodiscard]] std::string objectType() const override { return "Text"; }

    /// The point the text actually hangs from.
    ///
    /// When the alignment is anything but left/baseline, DWG uses the alignment
    /// point instead of the insertion point — a detail that misplaces text
    /// throughout a drawing if ignored.
    [[nodiscard]] Vec3 effectivePosition() const;

    /// Estimated advance width of the string, in drawing units.
    [[nodiscard]] double estimatedWidth() const {
        return measureText(contents, height, widthFactor);
    }

    [[nodiscard]] Box3 bounds() const override;

    /// Text is not curve geometry. Returning nothing keeps trim, fillet and the
    /// curve-based snaps from doing something nonsensical to it.
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override { return {}; }

    /// Picked by its bounding box rather than by an outline.
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;

protected:
    /// Copy the text-specific fields into `target`.
    ///
    /// Subclasses (ATTDEF and ATTRIB) call this from their own `clone` so the
    /// shared state is copied in one place without going back through `clone`,
    /// which would recurse.
    void copyTextInto(TextEntity& target) const;
    void restoreTextFrom(const TextEntity& snapshot);
};

/// Attachment point of an MTEXT block, matching DXF group code 71.
enum class MTextAttachment : std::uint8_t {
    TopLeft = 1,
    TopCenter = 2,
    TopRight = 3,
    MiddleLeft = 4,
    MiddleCenter = 5,
    MiddleRight = 6,
    BottomLeft = 7,
    BottomCenter = 8,
    BottomRight = 9,
};

/// Multi-line formatted text — MTEXT.
///
/// The contents carry other CAD software's inline formatting codes (`\P` for a paragraph
/// break, `{\fArial|b1;...}` for font runs). Those are preserved verbatim
/// rather than parsed into a rich model: a lossy parse would destroy formatting
/// that is not yet rendered.
class MTextEntity final : public Entity {
public:
    Vec3 position;
    /// Raw contents including inline formatting codes.
    std::string contents;
    double height = 2.5;
    /// Column width; 0 means the text does not wrap.
    double width = 0.0;
    double rotation = 0.0;
    MTextAttachment attachment = MTextAttachment::TopLeft;
    Handle styleHandle;
    double lineSpacingFactor = 1.0;
    std::optional<Vec3> directionVector;

    MTextEntity() = default;
    MTextEntity(const Vec3& where, std::string what, double textHeight)
        : position(where), contents(std::move(what)), height(textHeight) {}

    [[nodiscard]] std::string objectType() const override { return "MText"; }

    /// Contents with other CAD software's inline formatting codes removed.
    [[nodiscard]] std::string plainText() const;

    /// Contents split into stretches of uniform formatting.
    ///
    /// What a renderer that can honour bold, height and colour needs, and what
    /// `plainText` throws away. See MTextFormat.hpp.
    [[nodiscard]] std::vector<MTextRun> formattedRuns() const;

    /// Escape text so that it survives as *content* inside MText.
    ///
    /// The inverse of the stripping `plainText` does, for the three characters
    /// that are markup: a backslash and the two braces. Needed whenever text
    /// from elsewhere is placed into an MText — a field's value most of all,
    /// since a room name containing a brace would otherwise open a formatting
    /// scope and take the rest of the note with it.
    [[nodiscard]] static std::string escapeForContents(std::string_view text);

    /// Paragraph breaks as real newlines, for an editor to show.
    ///
    /// `\P` is the one MText code that is *layout the user is looking at*
    /// rather than formatting, so it is the one an editor can honestly turn
    /// into a line break. Everything else — colour, height, stacking — is left
    /// as written: showing a code is worse than hiding it only if you can also
    /// render it, and silently dropping the ones you cannot is how a note gets
    /// reformatted by being read.
    ///
    /// An escaped backslash is left alone, so a line ending in `\` does not
    /// acquire a paragraph break that was never there.
    [[nodiscard]] static std::string paragraphsToLines(std::string_view contents);

    /// The inverse, for writing an edited paragraph back.
    [[nodiscard]] static std::string linesToParagraphs(std::string_view text);

    [[nodiscard]] std::vector<std::string> lines() const;

    [[nodiscard]] Box3 bounds() const override;
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override { return {}; }
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

/// Flags shared by attribute definitions and attribute values.
struct AttributeFlags {
    /// Hidden from display but still stored.
    bool isInvisible = false;
    /// Value is fixed and not prompted for.
    bool isConstant = false;
    /// Value is verified with a second prompt.
    bool isVerifiable = false;
    /// Value is filled in automatically without prompting.
    bool isPreset = false;
    bool isMultiline = false;
    int fieldLength = 0;
};

/// An attribute definition inside a block — ATTDEF.
///
/// The *template*; each INSERT of the block carries AttributeEntity instances
/// holding the actual values. Two types rather than one with a mode flag,
/// matching DWG and keeping the distinction explicit.
class AttributeDefinitionEntity final : public TextEntity {
public:
    /// Programmatic name used to look the attribute up.
    std::string tag;
    /// Prompt shown when the block is inserted.
    std::string prompt;
    std::string defaultValue;
    AttributeFlags flags;

    [[nodiscard]] std::string objectType() const override { return "AttributeDefinition"; }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// An attribute value attached to a block reference — ATTRIB.
///
/// This is what carries the real data in a professional drawing: door numbers,
/// room names, equipment tags, title block fields. Preserving it accurately
/// matters as much as preserving geometry.
class AttributeEntity final : public TextEntity {
public:
    std::string tag;
    AttributeFlags flags;

    [[nodiscard]] std::string objectType() const override { return "Attribute"; }

    /// The attribute's value; stored in the inherited text contents.
    [[nodiscard]] const std::string& value() const noexcept { return contents; }
    void setValue(std::string v) { contents = std::move(v); }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
