#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace cad {

/// Colour model.
///
/// DWG colour is not a single RGB value. An entity's colour may be an index
/// into the 256-entry other CAD software Colour Index (ACI) palette, a 24-bit true colour,
/// or one of the two *inherited* pseudo-colours, ByLayer and ByBlock.
///
/// Modelling inheritance explicitly — rather than resolving to RGB at load
/// time — is essential: changing a layer's colour must visibly change every
/// ByLayer entity on it, and a block inserted on a different layer must
/// re-resolve its ByBlock entities. Flattening to RGB early destroys both.

enum class ColorMethod : std::uint8_t {
    ByLayer,
    ByBlock,
    Index,
    TrueColor,
};

/// ACI index reserved for ByBlock.
inline constexpr int kAciByBlock = 0;
/// ACI index reserved for ByLayer.
inline constexpr int kAciByLayer = 256;
/// ACI index meaning "no colour"; used by layers that are turned off.
inline constexpr int kAciNone = 257;

/// An immutable colour value.
class Color {
public:
    constexpr Color() noexcept = default;

    [[nodiscard]] static constexpr Color byLayer() noexcept {
        return Color{ColorMethod::ByLayer, 0, 0};
    }

    [[nodiscard]] static constexpr Color byBlock() noexcept {
        return Color{ColorMethod::ByBlock, 0, 0};
    }

    /// Colour from an ACI index, mapping the reserved indices to their meaning.
    [[nodiscard]] static Color fromIndex(int index) noexcept;

    [[nodiscard]] static constexpr Color fromTrueColor(std::uint32_t rgb) noexcept {
        return Color{ColorMethod::TrueColor, 0, rgb & 0xFFFFFFu};
    }

    [[nodiscard]] static constexpr Color fromRgb(int r, int g, int b) noexcept {
        return fromTrueColor(static_cast<std::uint32_t>(((r & 0xFF) << 16) | ((g & 0xFF) << 8) |
                                                        (b & 0xFF)));
    }

    [[nodiscard]] constexpr ColorMethod method() const noexcept { return method_; }
    [[nodiscard]] constexpr int index() const noexcept { return index_; }
    [[nodiscard]] constexpr std::uint32_t rgb() const noexcept { return rgb_; }

    [[nodiscard]] constexpr bool isByLayer() const noexcept {
        return method_ == ColorMethod::ByLayer;
    }

    [[nodiscard]] constexpr bool isByBlock() const noexcept {
        return method_ == ColorMethod::ByBlock;
    }

    /// True when this colour needs no context to resolve to RGB.
    [[nodiscard]] constexpr bool isConcrete() const noexcept {
        return method_ == ColorMethod::Index || method_ == ColorMethod::TrueColor;
    }

    /// Resolve to a packed 0xRRGGBB value.
    ///
    /// Only meaningful for concrete colours; inherited colours must be resolved
    /// against their layer or block first. An unresolved inherited colour
    /// renders as the ACI 7 default rather than throwing — a drawing must
    /// always be displayable.
    [[nodiscard]] std::uint32_t toRgb() const noexcept;

    /// Optional colour-book name, preserved verbatim for round-tripping.
    std::string bookName;
    std::string colorName;

    [[nodiscard]] constexpr bool equals(const Color& other) const noexcept {
        return method_ == other.method_ && index_ == other.index_ && rgb_ == other.rgb_;
    }

    [[nodiscard]] std::string toString() const;

private:
    constexpr Color(ColorMethod method, int index, std::uint32_t rgb) noexcept
        : method_(method), index_(index), rgb_(rgb) {}

    ColorMethod method_ = ColorMethod::ByLayer;
    int index_ = 0;
    std::uint32_t rgb_ = 0;
};

/// RGB for an ACI index. Out-of-range indices fall back to white.
[[nodiscard]] std::uint32_t aciToRgb(int index) noexcept;

/// Nearest ACI index to a true colour, for export to index-only contexts.
[[nodiscard]] int rgbToAci(std::uint32_t rgb) noexcept;

/// Lineweight, in hundredths of a millimetre — the DWG unit.
///
/// The negative values are sentinels with the same inheritance role that
/// ByLayer and ByBlock play for colour.
enum class Lineweight : std::int16_t {
    ByLayer = -1,
    ByBlock = -2,
    Default = -3,
};

[[nodiscard]] constexpr std::int16_t toRaw(Lineweight weight) noexcept {
    return static_cast<std::int16_t>(weight);
}

/// The fixed set of lineweights other CAD software permits, in hundredths of a mm.
[[nodiscard]] std::span<const std::int16_t> standardLineweights() noexcept;

/// Snap an arbitrary width to the nearest permitted lineweight.
[[nodiscard]] std::int16_t normalizeLineweight(int value) noexcept;

/// Transparency, stored as DWG's 0-255 alpha where 255 is fully opaque.
/// Empty means ByLayer.
using Transparency = std::optional<std::uint8_t>;

inline constexpr std::uint8_t kTransparencyOpaque = 255;

}  // namespace cad
