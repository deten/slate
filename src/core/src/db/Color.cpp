#include "cad/db/Color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace cad {
namespace {

/// The named primaries of other CAD software Colour Index palette.
constexpr std::array<std::uint32_t, 10> kAciPrimaries{
    0x000000,  // 0 - ByBlock placeholder
    0xFF0000,  // 1 red
    0xFFFF00,  // 2 yellow
    0x00FF00,  // 3 green
    0x00FFFF,  // 4 cyan
    0x0000FF,  // 5 blue
    0xFF00FF,  // 6 magenta
    0xFFFFFF,  // 7 white/black, rendered to contrast with the background
    0x808080,  // 8 dark grey
    0xC0C0C0,  // 9 light grey
};

/// Hue sequence for the 10-249 ramp, in degrees.
constexpr std::array<int, 24> kRampHues{0,   15,  30,  45,  60,  75,  90,  105,
                                        120, 135, 150, 165, 180, 195, 210, 225,
                                        240, 255, 270, 285, 300, 315, 330, 345};

/// Value/saturation pairs for the five shades within each hue.
constexpr std::array<std::pair<double, double>, 5> kRampShades{
    {{1.0, 1.0}, {1.0, 0.5}, {0.76, 1.0}, {0.76, 0.5}, {0.5, 1.0}}};

[[nodiscard]] std::uint32_t hsvToRgb(double h, double s, double v) noexcept {
    const double c = v * s;
    const double hp = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 60.0;
    const double x = c * (1.0 - std::abs(std::fmod(hp, 2.0) - 1.0));

    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    if (hp < 1.0) {
        r = c;
        g = x;
    } else if (hp < 2.0) {
        r = x;
        g = c;
    } else if (hp < 3.0) {
        g = c;
        b = x;
    } else if (hp < 4.0) {
        g = x;
        b = c;
    } else if (hp < 5.0) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }

    const double m = v - c;
    const auto to8 = [m](double n) {
        return static_cast<std::uint32_t>(std::lround((n + m) * 255.0)) & 0xFFu;
    };
    return (to8(r) << 16) | (to8(g) << 8) | to8(b);
}

/// Build the 258-entry palette once, on first use.
[[nodiscard]] const std::array<std::uint32_t, 258>& palette() noexcept {
    static const std::array<std::uint32_t, 258> table = [] {
        std::array<std::uint32_t, 258> result{};
        for (std::size_t i = 0; i < kAciPrimaries.size(); ++i) {
            result[i] = kAciPrimaries[i];
        }

        // 10-249: 24 hues, each alternating a bright and a half-luminance
        // variant across five shade steps.
        std::size_t index = 10;
        for (const int hue : kRampHues) {
            for (const auto& [value, saturation] : kRampShades) {
                if (index > 249) {
                    break;
                }
                result[index++] = hsvToRgb(hue, saturation, value);
                if (index > 249) {
                    break;
                }
                result[index++] = hsvToRgb(hue, saturation, value * 0.55);
            }
        }

        // 250-255: greyscale ramp from near-black to white.
        constexpr std::array<std::uint32_t, 6> kGreys{0x333333, 0x505050, 0x696969,
                                                      0x828282, 0xBEBEBE, 0xFFFFFF};
        for (std::size_t i = 0; i < kGreys.size(); ++i) {
            result[250 + i] = kGreys[i];
        }
        result[256] = 0xFFFFFF;  // ByLayer placeholder
        result[257] = 0x000000;  // None
        return result;
    }();
    return table;
}

constexpr std::array<std::int16_t, 24> kStandardLineweights{
    0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120, 140, 158,
    200, 211};

}  // namespace

Color Color::fromIndex(int index) noexcept {
    if (index == kAciByBlock) {
        return byBlock();
    }
    if (index == kAciByLayer) {
        return byLayer();
    }
    return Color{ColorMethod::Index, std::clamp(index, 0, 257), 0};
}

std::uint32_t Color::toRgb() const noexcept {
    switch (method_) {
        case ColorMethod::TrueColor:
            return rgb_;
        case ColorMethod::Index:
            return aciToRgb(index_);
        case ColorMethod::ByLayer:
        case ColorMethod::ByBlock:
            break;
    }
    // An unresolved inherited colour renders as the ACI 7 default rather than
    // failing: a drawing must always be displayable.
    return aciToRgb(7);
}

std::string Color::toString() const {
    switch (method_) {
        case ColorMethod::ByLayer:
            return "ByLayer";
        case ColorMethod::ByBlock:
            return "ByBlock";
        case ColorMethod::Index:
            return "ACI " + std::to_string(index_);
        case ColorMethod::TrueColor:
            break;
    }
    if (!colorName.empty()) {
        return colorName;
    }
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%06X", rgb_ & 0xFFFFFFu);
    return std::string{buffer};
}

std::uint32_t aciToRgb(int index) noexcept {
    if (index < 0 || index > 257) {
        return 0xFFFFFF;
    }
    return palette()[static_cast<std::size_t>(index)];
}

int rgbToAci(std::uint32_t rgb) noexcept {
    const auto& table = palette();
    const auto r = static_cast<int>((rgb >> 16) & 0xFF);
    const auto g = static_cast<int>((rgb >> 8) & 0xFF);
    const auto b = static_cast<int>(rgb & 0xFF);

    int best = 7;
    long long bestDistance = std::numeric_limits<long long>::max();
    // Skip the reserved 0, 256 and 257 slots.
    for (int i = 1; i <= 255; ++i) {
        const std::uint32_t candidate = table[static_cast<std::size_t>(i)];
        const long long dr = static_cast<int>((candidate >> 16) & 0xFF) - r;
        const long long dg = static_cast<int>((candidate >> 8) & 0xFF) - g;
        const long long db = static_cast<int>(candidate & 0xFF) - b;
        const long long distance = dr * dr + dg * dg + db * db;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

std::span<const std::int16_t> standardLineweights() noexcept {
    return kStandardLineweights;
}

std::int16_t normalizeLineweight(int value) noexcept {
    if (value < 0) {
        const auto raw = static_cast<std::int16_t>(value);
        return (raw == toRaw(Lineweight::ByBlock) || raw == toRaw(Lineweight::Default))
                   ? raw
                   : toRaw(Lineweight::ByLayer);
    }
    std::int16_t best = kStandardLineweights.front();
    int bestDistance = std::numeric_limits<int>::max();
    for (const std::int16_t candidate : kStandardLineweights) {
        const int distance = std::abs(candidate - value);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

}  // namespace cad
