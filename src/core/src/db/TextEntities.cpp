#include "cad/db/TextEntities.hpp"

#include <algorithm>
#include <stdexcept>

namespace cad {
namespace {

/// The 0.6 factor is the average glyph aspect ratio of the `txt.shx` setting in other CAD software and
/// of common sans-serif faces. A placeholder for real font metrics, used only
/// in headless contexts.
constexpr double kDefaultAspect = 0.6;

TextMeasurer& measurer() {
    static TextMeasurer instance = [](std::string_view text, double height,
                                      double widthFactor) {
        return static_cast<double>(text.size()) * height * kDefaultAspect * widthFactor;
    };
    return instance;
}

template <typename T>
[[nodiscard]] const T& expectSnapshot(const DbObject& snapshot, const char* what) {
    const auto* source = dynamic_cast<const T*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument(std::string{"Snapshot type mismatch restoring "} + what);
    }
    return *source;
}

}  // namespace

void setTextMeasurer(TextMeasurer newMeasurer) {
    if (newMeasurer) {
        measurer() = std::move(newMeasurer);
    }
}

double measureText(std::string_view text, double height, double widthFactor) {
    return measurer()(text, height, widthFactor);
}

// ------------------------------------------------------------------ TextEntity

Vec3 TextEntity::effectivePosition() const {
    const bool usesAlignmentPoint =
        horizontalAlignment != TextHorizontalAlignment::Left ||
        verticalAlignment != TextVerticalAlignment::Baseline;
    return (usesAlignmentPoint && alignmentPoint) ? *alignmentPoint : position;
}

Box3 TextEntity::bounds() const {
    const double textWidth = estimatedWidth();
    const Vec3 anchor = effectivePosition();

    // Local-space box relative to the anchor, before rotation.
    double left = 0.0;
    switch (horizontalAlignment) {
        case TextHorizontalAlignment::Center:
        case TextHorizontalAlignment::Middle:
            left = -textWidth / 2.0;
            break;
        case TextHorizontalAlignment::Right:
            left = -textWidth;
            break;
        default:
            left = 0.0;
            break;
    }

    double bottom = 0.0;
    switch (verticalAlignment) {
        case TextVerticalAlignment::Top:
            bottom = -height;
            break;
        case TextVerticalAlignment::Middle:
            bottom = -height / 2.0;
            break;
        case TextVerticalAlignment::Bottom:
            bottom = 0.0;
            break;
        default:
            // Baseline: descenders fall below the insertion point.
            bottom = -height * 0.25;
            break;
    }

    const Box3 local{Vec3{left, bottom, 0.0}, Vec3{left + textWidth, bottom + height, 0.0}};
    return local.transformed(Mat4::rotationZ(rotation)).transformed(Mat4::translation(anchor));
}

double TextEntity::distanceToPoint(const Vec3& p) const {
    const Box3 box = bounds();
    return box.containsPoint(p) ? 0.0 : box.distanceToPoint(p);
}

void TextEntity::transformBy(const Mat4& m) {
    position = m.transformPoint(position);
    if (alignmentPoint) {
        alignmentPoint = m.transformPoint(*alignmentPoint);
    }
    // Text is never sheared by other CAD software, so the conformal components suffice.
    height *= m.scaleFactor2();
    rotation += m.rotation2();
    if (m.isMirrored()) {
        isBackwards = !isBackwards;
    }
}

std::vector<GripPoint> TextEntity::gripPoints() const {
    return {{effectivePosition(), GripKind::Insertion, 0}};
}

void TextEntity::moveGrip(const GripPoint& /*grip*/, const Vec3& to) {
    const Vec3 delta = to - effectivePosition();
    position += delta;
    if (alignmentPoint) {
        alignmentPoint = *alignmentPoint + delta;
    }
}

void TextEntity::copyTextInto(TextEntity& target) const {
    copyEntityInto(target);
    target.position = position;
    target.alignmentPoint = alignmentPoint;
    target.contents = contents;
    target.height = height;
    target.rotation = rotation;
    target.widthFactor = widthFactor;
    target.obliqueAngle = obliqueAngle;
    target.styleHandle = styleHandle;
    target.horizontalAlignment = horizontalAlignment;
    target.verticalAlignment = verticalAlignment;
    target.isBackwards = isBackwards;
    target.isUpsideDown = isUpsideDown;
    target.thickness = thickness;
}

void TextEntity::restoreTextFrom(const TextEntity& snapshot) {
    restoreEntityFrom(snapshot);
    position = snapshot.position;
    alignmentPoint = snapshot.alignmentPoint;
    contents = snapshot.contents;
    height = snapshot.height;
    rotation = snapshot.rotation;
    widthFactor = snapshot.widthFactor;
    obliqueAngle = snapshot.obliqueAngle;
    styleHandle = snapshot.styleHandle;
    horizontalAlignment = snapshot.horizontalAlignment;
    verticalAlignment = snapshot.verticalAlignment;
    isBackwards = snapshot.isBackwards;
    isUpsideDown = snapshot.isUpsideDown;
    thickness = snapshot.thickness;
}

DbObjectPtr TextEntity::clone() const {
    auto copy = std::make_unique<TextEntity>();
    copyTextInto(*copy);
    return copy;
}

void TextEntity::restoreFrom(const DbObject& snapshot) {
    restoreTextFrom(expectSnapshot<TextEntity>(snapshot, "text"));
}

std::vector<Handle> TextEntity::referencedHandles() const {
    std::vector<Handle> refs = Entity::referencedHandles();
    if (!styleHandle.isNull()) {
        refs.push_back(styleHandle);
    }
    return refs;
}

// ----------------------------------------------------------------- MTextEntity

std::string MTextEntity::paragraphsToLines(std::string_view contents) {
    std::string out;
    out.reserve(contents.size());
    for (std::size_t i = 0; i < contents.size(); ++i) {
        if (contents[i] == '\\' && i + 1 < contents.size()) {
            if (contents[i + 1] == 'P') {
                out.push_back('\n');
                ++i;
                continue;
            }
            if (contents[i + 1] == '\\') {
                // An escaped backslash passes through as a pair, so a line
                // ending in one does not acquire a break that was never there.
                out.append("\\\\");
                ++i;
                continue;
            }
        }
        out.push_back(contents[i]);
    }
    return out;
}

std::string MTextEntity::linesToParagraphs(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        if (character == '\n') {
            out.append("\\P");
        } else if (character != '\r') {
            // A stray carriage return would become a literal character in the
            // drawing, which renders as a box in most fonts.
            out.push_back(character);
        }
    }
    return out;
}

std::string MTextEntity::escapeForContents(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\\' || ch == '{' || ch == '}') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::vector<MTextRun> MTextEntity::formattedRuns() const {
    return parseMText(contents);
}

std::string MTextEntity::plainText() const {
    // Declared in the io layer, but MTEXT formatting is a property of the
    // format rather than of I/O, so the stripping lives with the entity.
    std::string out;
    out.reserve(contents.size());

    for (std::size_t i = 0; i < contents.size(); ++i) {
        const char ch = contents[i];

        if (ch == '\\' && i + 1 < contents.size()) {
            const char next = contents[i + 1];

            // Escaped literal brace or backslash.
            if (next == '\\' || next == '{' || next == '}') {
                out.push_back(next);
                ++i;
                continue;
            }
            if (next == 'P') {  // Paragraph break.
                out.push_back('\n');
                ++i;
                continue;
            }
            if (next == '~') {  // Non-breaking space.
                out.push_back(' ');
                ++i;
                continue;
            }
            // Stacked text: \S numerator ^ denominator ;
            if (next == 'S') {
                const std::size_t end = contents.find(';', i + 2);
                std::string body = contents.substr(
                    i + 2, end == std::string::npos ? std::string::npos : end - (i + 2));
                const std::size_t separator = body.find_first_of("^#");
                if (separator != std::string::npos) {
                    body[separator] = '/';
                }
                out += body;
                i = (end == std::string::npos) ? contents.size() : end;
                continue;
            }
            // Codes terminated by a semicolon: font, colour, height, width.
            // Toggles: no argument, no terminator. `L` used to be in the list
            // below, so an underlined run was swallowed to the next semicolon.
            if (std::string_view{"LlOoKk"}.find(next) != std::string_view::npos) {
                ++i;
                continue;
            }
            if (std::string_view{"fFcCHWTAQp"}.find(next) != std::string_view::npos) {
                const std::size_t end = contents.find(';', i + 2);
                i = (end == std::string::npos) ? contents.size() : end;
                continue;
            }
            // Any other escape: drop the code character.
            ++i;
            continue;
        }

        // Braces group formatting scopes and are not literal content.
        if (ch == '{' || ch == '}') {
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::vector<std::string> MTextEntity::lines() const {
    const std::string plain = plainText();
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const std::size_t newline = plain.find('\n', start);
        if (newline == std::string::npos) {
            result.push_back(plain.substr(start));
            break;
        }
        result.push_back(plain.substr(start, newline - start));
        start = newline + 1;
    }
    return result;
}

Box3 MTextEntity::bounds() const {
    const std::vector<std::string> textLines = lines();
    const double lineHeight = height * lineSpacingFactor;
    const double totalHeight = static_cast<double>(std::max<std::size_t>(textLines.size(), 1)) *
                               lineHeight;

    // `width` is the *wrap* rectangle, not the text's extent, and the two are
    // only the same for a paragraph long enough to fill it. A short label with
    // a wide defined width — or, as here, a long one in a narrow rectangle —
    // needs the measured text as well, or the box misses the end of its own
    // label and clicking the text selects nothing.
    //
    // The maximum of the two is what is actually drawn: nothing below this
    // layer wraps, so a line runs to its full length whatever the rectangle
    // says. Revisit alongside wrapping, which would cap it at `width` again.
    double widest = std::max(width, 0.0);
    for (const std::string& line : textLines) {
        widest = std::max(widest, measureText(line, height, 1.0));
    }

    // Offset from the attachment point to the box's lower-left corner.
    const int attachmentIndex = static_cast<int>(attachment) - 1;
    const int column = attachmentIndex % 3;
    const int row = attachmentIndex / 3;

    const double left = column == 0 ? 0.0 : (column == 1 ? -widest / 2.0 : -widest);
    const double bottom =
        row == 0 ? -totalHeight : (row == 1 ? -totalHeight / 2.0 : 0.0);

    const Box3 local{Vec3{left, bottom, 0.0},
                     Vec3{left + widest, bottom + totalHeight, 0.0}};
    return local.transformed(Mat4::rotationZ(rotation))
        .transformed(Mat4::translation(position));
}

double MTextEntity::distanceToPoint(const Vec3& p) const {
    const Box3 box = bounds();
    return box.containsPoint(p) ? 0.0 : box.distanceToPoint(p);
}

void MTextEntity::transformBy(const Mat4& m) {
    position = m.transformPoint(position);
    const double scale = m.scaleFactor2();
    height *= scale;
    width *= scale;
    rotation += m.rotation2();
    if (directionVector) {
        directionVector = m.transformVector(*directionVector);
    }
}

std::vector<GripPoint> MTextEntity::gripPoints() const {
    return {{position, GripKind::Insertion, 0}};
}

void MTextEntity::moveGrip(const GripPoint& /*grip*/, const Vec3& to) {
    position = to;
}

DbObjectPtr MTextEntity::clone() const {
    auto copy = std::make_unique<MTextEntity>();
    copyEntityInto(*copy);
    copy->position = position;
    copy->contents = contents;
    copy->height = height;
    copy->width = width;
    copy->rotation = rotation;
    copy->attachment = attachment;
    copy->styleHandle = styleHandle;
    copy->lineSpacingFactor = lineSpacingFactor;
    copy->directionVector = directionVector;
    return copy;
}

void MTextEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<MTextEntity>(snapshot, "mtext");
    restoreEntityFrom(source);
    position = source.position;
    contents = source.contents;
    height = source.height;
    width = source.width;
    rotation = source.rotation;
    attachment = source.attachment;
    styleHandle = source.styleHandle;
    lineSpacingFactor = source.lineSpacingFactor;
    directionVector = source.directionVector;
}

std::vector<Handle> MTextEntity::referencedHandles() const {
    std::vector<Handle> refs = Entity::referencedHandles();
    if (!styleHandle.isNull()) {
        refs.push_back(styleHandle);
    }
    return refs;
}

// ------------------------------------------------------- Attribute definitions

DbObjectPtr AttributeDefinitionEntity::clone() const {
    auto copy = std::make_unique<AttributeDefinitionEntity>();
    copyTextInto(*copy);
    copy->tag = tag;
    copy->prompt = prompt;
    copy->defaultValue = defaultValue;
    copy->flags = flags;
    return copy;
}

void AttributeDefinitionEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source =
        expectSnapshot<AttributeDefinitionEntity>(snapshot, "an attribute definition");
    restoreTextFrom(source);
    tag = source.tag;
    prompt = source.prompt;
    defaultValue = source.defaultValue;
    flags = source.flags;
}

DbObjectPtr AttributeEntity::clone() const {
    auto copy = std::make_unique<AttributeEntity>();
    copyTextInto(*copy);
    copy->tag = tag;
    copy->flags = flags;
    return copy;
}

void AttributeEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<AttributeEntity>(snapshot, "an attribute");
    restoreTextFrom(source);
    tag = source.tag;
    flags = source.flags;
}

}  // namespace cad
