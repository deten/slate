#include "cad/db/DimStyle.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <stdexcept>

#include "cad/math/Angle.hpp"

namespace cad {
namespace {

/// Fixed-notation formatting with an explicit number of decimals.
///
/// `std::to_chars` rather than a stream: it is locale-independent, and a
/// dimension that reads `3,50` on a German machine and `3.50` elsewhere is a
/// drawing that does not round-trip. The decimal separator is applied
/// afterwards, from the style, which is where the choice belongs.
///
/// The value is pre-rounded half *away from zero*. `to_chars` — like `printf`
/// — rounds half to even, so 42.5 at zero places becomes 42. That is the right
/// rule for statistics and the wrong one for a drawing: a dimension reading 42
/// for a 42.5 part is wrong in a way the reader cannot detect, and it is not
/// what other CAD software writes.
[[nodiscard]] std::string fixed(double value, int decimals) {
    const int places = std::clamp(decimals, 0, 16);
    const double factor = std::pow(10.0, places);

    double rounded = value;
    // Skipped when scaling would lose precision outright, since at that
    // magnitude there is no fractional part left to round.
    if (std::isfinite(value) && std::abs(value) < 1e15 / factor) {
        rounded = std::round(value * factor) / factor;
    }

    char buffer[64];
    const auto [pointer, error] =
        std::to_chars(buffer, buffer + sizeof(buffer), rounded, std::chars_format::fixed, places);
    if (error != std::errc{}) {
        return "0";
    }
    return std::string{buffer, pointer};
}

/// Strip zeros the style says to leave off.
void applyZeroSuppression(std::string& text, const ZeroSuppression& zeros) {
    const std::size_t point = text.find('.');
    if (point == std::string::npos) {
        return;
    }
    if (zeros.suppressTrailing) {
        std::size_t end = text.size();
        while (end > point + 1 && text[end - 1] == '0') {
            --end;
        }
        // Leave `12.` behind as `12`, never as `12.`.
        if (end == point + 1) {
            end = point;
        }
        text.erase(end);
    }
    if (zeros.suppressLeading) {
        // Only the units digit goes, and only when it is a lone zero: `0.5`
        // becomes `.5` but `10.5` must not become `1.5`.
        if (text.size() >= 2 && text[0] == '0' && text[1] == '.') {
            text.erase(0, 1);
        } else if (text.size() >= 3 && text[0] == '-' && text[1] == '0' && text[2] == '.') {
            text.erase(1, 1);
        }
    }
}

void applySeparator(std::string& text, char separator) {
    if (separator == '.' || separator == '\0') {
        return;
    }
    const std::size_t point = text.find('.');
    if (point != std::string::npos) {
        text[point] = separator;
    }
}

/// Reduce `numerator / denominator` to lowest terms.
void reduce(long long& numerator, long long& denominator) {
    if (numerator == 0) {
        denominator = 1;
        return;
    }
    const long long divisor = std::gcd(numerator, denominator);
    if (divisor > 1) {
        numerator /= divisor;
        denominator /= divisor;
    }
}

/// The fraction denominator DIMDEC selects in the fractional formats.
///
/// Unlike the decimal formats, where DIMDEC counts decimal places, here it is
/// an exponent: 4 means sixteenths. Getting this wrong produces dimensions that
/// are plausible and wrong, which is the worst kind.
[[nodiscard]] long long fractionDenominator(int precision) {
    return 1LL << std::clamp(precision, 0, 8);
}

/// `1 1/2`, `1/2`, or `2` — whichever the value actually is.
[[nodiscard]] std::string mixedFraction(double value, int precision, bool suppressZeroWhole) {
    const long long denominator = fractionDenominator(precision);
    auto units = static_cast<long long>(std::floor(value));
    auto numerator =
        static_cast<long long>(std::llround((value - static_cast<double>(units)) *
                                            static_cast<double>(denominator)));
    if (numerator >= denominator) {
        // Rounding pushed the fraction up to a whole unit.
        numerator -= denominator;
        ++units;
    }

    long long reducedNumerator = numerator;
    long long reducedDenominator = denominator;
    reduce(reducedNumerator, reducedDenominator);

    std::string text;
    if (units != 0 || reducedNumerator == 0 || !suppressZeroWhole) {
        text += std::to_string(units);
    }
    if (reducedNumerator != 0) {
        if (!text.empty()) {
            text += ' ';
        }
        text += std::to_string(reducedNumerator);
        text += '/';
        text += std::to_string(reducedDenominator);
    }
    return text;
}

/// Feet and inches, for the engineering and architectural formats.
///
/// Both assume the drawing unit is one inch, which is the convention these
/// formats exist to serve; a metric drawing has no business using them.
[[nodiscard]] std::string feetAndInches(double inches, const DimStyle& style, bool fractional) {
    const bool negative = inches < 0.0;
    const double magnitude = std::abs(inches);

    auto feet = static_cast<long long>(std::floor(magnitude / 12.0));
    double remainder = magnitude - static_cast<double>(feet) * 12.0;

    // Round the inches part first: 11.9999" must become 1'-0", not 0'-12".
    if (fractional) {
        const auto denominator = static_cast<double>(fractionDenominator(style.linearPrecision));
        remainder = std::round(remainder * denominator) / denominator;
    } else {
        const double factor = std::pow(10.0, std::clamp(style.linearPrecision, 0, 8));
        remainder = std::round(remainder * factor) / factor;
    }
    if (remainder >= 12.0) {
        remainder -= 12.0;
        ++feet;
    }

    const ZeroSuppression& zeros = style.linearZeroSuppression;
    const bool hasInches = remainder > 0.0;

    std::string text;
    if (feet != 0 || !zeros.suppressZeroFeet || (!hasInches && !zeros.suppressZeroInches)) {
        text += std::to_string(feet);
        text += '\'';
    }

    if (hasInches || !zeros.suppressZeroInches) {
        if (!text.empty()) {
            text += '-';
        }
        if (fractional) {
            text += mixedFraction(remainder, style.linearPrecision, false);
        } else {
            std::string inchText = fixed(remainder, style.linearPrecision);
            applyZeroSuppression(inchText, zeros);
            text += inchText;
        }
        text += '"';
    }

    if (text.empty()) {
        // Everything was suppressed, which cannot be the answer.
        text = "0\"";
    }
    return negative ? "-" + text : text;
}

/// Apply DIMPOST.
///
/// `<>` marks where the measurement goes; without it the whole string is a
/// suffix. That asymmetry is other CAD software's, and files depend on it.
[[nodiscard]] std::string applyPostfix(const std::string& postfix, const std::string& measurement) {
    if (postfix.empty()) {
        return measurement;
    }
    const std::size_t marker = postfix.find("<>");
    if (marker == std::string::npos) {
        return measurement + postfix;
    }
    return postfix.substr(0, marker) + measurement + postfix.substr(marker + 2);
}

}  // namespace

// ------------------------------------------------------------ zero suppression

ZeroSuppression ZeroSuppression::fromDimzin(int dimzin) noexcept {
    ZeroSuppression zeros;
    // The low two bits are an enumeration over the feet/inches combinations,
    // not two independent flags — a genuine trap when reading the DXF docs.
    switch (dimzin & 3) {
        case 1: zeros.suppressZeroFeet = false; zeros.suppressZeroInches = false; break;
        case 2: zeros.suppressZeroFeet = false; zeros.suppressZeroInches = true; break;
        case 3: zeros.suppressZeroFeet = true; zeros.suppressZeroInches = false; break;
        case 0:
        default: zeros.suppressZeroFeet = true; zeros.suppressZeroInches = true; break;
    }
    zeros.suppressLeading = (dimzin & 4) != 0;
    zeros.suppressTrailing = (dimzin & 8) != 0;
    return zeros;
}

int ZeroSuppression::toDimzin() const noexcept {
    int value = 0;
    if (suppressZeroFeet && suppressZeroInches) {
        value = 0;
    } else if (!suppressZeroFeet && !suppressZeroInches) {
        value = 1;
    } else if (!suppressZeroFeet) {
        value = 2;
    } else {
        value = 3;
    }
    if (suppressLeading) { value |= 4; }
    if (suppressTrailing) { value |= 8; }
    return value;
}

// -------------------------------------------------------------------- record

DbObjectPtr DimStyle::clone() const {
    auto copy = std::make_unique<DimStyle>();
    copyRecordInto(*copy);
    copy->overallScale = overallScale;

    copy->dimensionLineColor = dimensionLineColor;
    copy->extensionLineColor = extensionLineColor;
    copy->dimensionLineWeight = dimensionLineWeight;
    copy->extensionLineWeight = extensionLineWeight;
    copy->dimensionLineExtend = dimensionLineExtend;
    copy->baselineSpacing = baselineSpacing;
    copy->suppressDimensionLine1 = suppressDimensionLine1;
    copy->suppressDimensionLine2 = suppressDimensionLine2;
    copy->extensionLineExtend = extensionLineExtend;
    copy->extensionLineOffset = extensionLineOffset;
    copy->suppressExtensionLine1 = suppressExtensionLine1;
    copy->suppressExtensionLine2 = suppressExtensionLine2;
    copy->fixedExtensionLines = fixedExtensionLines;
    copy->fixedExtensionLength = fixedExtensionLength;

    copy->arrowSize = arrowSize;
    copy->arrowBlock = arrowBlock;
    copy->arrowBlock1 = arrowBlock1;
    copy->arrowBlock2 = arrowBlock2;
    copy->separateArrowBlocks = separateArrowBlocks;
    copy->centerMarkSize = centerMarkSize;

    copy->textStyleHandle = textStyleHandle;
    copy->textHeight = textHeight;
    copy->textColor = textColor;
    copy->textGap = textGap;
    copy->textVerticalPosition = textVerticalPosition;
    copy->textHorizontalPosition = textHorizontalPosition;
    copy->textInsideHorizontal = textInsideHorizontal;
    copy->textOutsideHorizontal = textOutsideHorizontal;
    copy->textHasBackground = textHasBackground;
    copy->textBackgroundColor = textBackgroundColor;

    copy->linearScale = linearScale;
    copy->roundOff = roundOff;
    copy->linearFormat = linearFormat;
    copy->linearPrecision = linearPrecision;
    copy->decimalSeparator = decimalSeparator;
    copy->linearZeroSuppression = linearZeroSuppression;
    copy->linearPostfix = linearPostfix;

    copy->angularFormat = angularFormat;
    copy->angularPrecision = angularPrecision;
    copy->angularZeroSuppression = angularZeroSuppression;
    return copy;
}

void DimStyle::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const DimStyle*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a dimension style");
    }
    restoreRecordFrom(*source);

    overallScale = source->overallScale;
    dimensionLineColor = source->dimensionLineColor;
    extensionLineColor = source->extensionLineColor;
    dimensionLineWeight = source->dimensionLineWeight;
    extensionLineWeight = source->extensionLineWeight;
    dimensionLineExtend = source->dimensionLineExtend;
    baselineSpacing = source->baselineSpacing;
    suppressDimensionLine1 = source->suppressDimensionLine1;
    suppressDimensionLine2 = source->suppressDimensionLine2;
    extensionLineExtend = source->extensionLineExtend;
    extensionLineOffset = source->extensionLineOffset;
    suppressExtensionLine1 = source->suppressExtensionLine1;
    suppressExtensionLine2 = source->suppressExtensionLine2;
    fixedExtensionLines = source->fixedExtensionLines;
    fixedExtensionLength = source->fixedExtensionLength;

    arrowSize = source->arrowSize;
    arrowBlock = source->arrowBlock;
    arrowBlock1 = source->arrowBlock1;
    arrowBlock2 = source->arrowBlock2;
    separateArrowBlocks = source->separateArrowBlocks;
    centerMarkSize = source->centerMarkSize;

    textStyleHandle = source->textStyleHandle;
    textHeight = source->textHeight;
    textColor = source->textColor;
    textGap = source->textGap;
    textVerticalPosition = source->textVerticalPosition;
    textHorizontalPosition = source->textHorizontalPosition;
    textInsideHorizontal = source->textInsideHorizontal;
    textOutsideHorizontal = source->textOutsideHorizontal;
    textHasBackground = source->textHasBackground;
    textBackgroundColor = source->textBackgroundColor;

    linearScale = source->linearScale;
    roundOff = source->roundOff;
    linearFormat = source->linearFormat;
    linearPrecision = source->linearPrecision;
    decimalSeparator = source->decimalSeparator;
    linearZeroSuppression = source->linearZeroSuppression;
    linearPostfix = source->linearPostfix;

    angularFormat = source->angularFormat;
    angularPrecision = source->angularPrecision;
    angularZeroSuppression = source->angularZeroSuppression;
}

std::vector<Handle> DimStyle::referencedHandles() const {
    std::vector<Handle> handles = DbObject::referencedHandles();
    for (const Handle referenced : {textStyleHandle, arrowBlock, arrowBlock1, arrowBlock2}) {
        if (!referenced.isNull()) {
            handles.push_back(referenced);
        }
    }
    return handles;
}

// ------------------------------------------------------------------ overrides

namespace {

/// XData group codes that carry a value, as opposed to structure.
[[nodiscard]] bool isValueCode(int code) {
    return code == 1000 || code == 1005 || code == 1040 || code == 1070 || code == 1071;
}

[[nodiscard]] std::optional<double> asNumber(const XDataValue& value) {
    if (const auto* real = std::get_if<double>(&value)) { return *real; }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*integer);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> asInteger(const XDataValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) { return *integer; }
    if (const auto* real = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(std::llround(*real));
    }
    return std::nullopt;
}

/// The items of the ACAD DSTYLE block, or empty when there is none.
[[nodiscard]] std::vector<XDataItem> dstyleItems(const DbObject& object) {
    const std::vector<XDataItem>* acad = object.getXData("ACAD");
    if (acad == nullptr) {
        return {};
    }

    std::vector<XDataItem> items;
    bool inBlock = false;
    int depth = 0;

    for (std::size_t i = 0; i < acad->size(); ++i) {
        const XDataItem& item = (*acad)[i];

        // The marker is a 1000 string immediately before the opening brace.
        if (!inBlock && item.code == 1000) {
            const auto* text = std::get_if<std::string>(&item.value);
            if (text != nullptr && *text == "DSTYLE" && i + 1 < acad->size() &&
                (*acad)[i + 1].code == 1002) {
                inBlock = true;
                continue;
            }
        }
        if (!inBlock) {
            continue;
        }

        if (item.code == 1002) {
            const auto* brace = std::get_if<std::string>(&item.value);
            const bool opening = brace != nullptr && !brace->empty() && brace->front() == '{';
            depth += opening ? 1 : -1;
            if (depth <= 0) {
                break;
            }
            continue;
        }
        items.push_back(item);
    }
    return items;
}

}  // namespace

bool hasDimStyleOverrides(const DbObject& object) {
    return !dstyleItems(object).empty();
}

std::size_t applyDimStyleOverrides(DimStyle& style, const DbObject& object) {
    const std::vector<XDataItem> items = dstyleItems(object);
    std::size_t applied = 0;

    // Pairs: a 1070 naming the variable, then whatever code carries its value.
    for (std::size_t i = 0; i + 1 < items.size(); ++i) {
        if (items[i].code != 1070) {
            continue;
        }
        const auto variable = asInteger(items[i].value);
        if (!variable) {
            continue;
        }
        const XDataItem& valueItem = items[i + 1];
        if (!isValueCode(valueItem.code)) {
            continue;
        }
        ++i;  // The value belongs to this variable, not to the next one.

        const auto number = asNumber(valueItem.value);
        const auto integer = asInteger(valueItem.value);
        const auto* text = std::get_if<std::string>(&valueItem.value);
        bool handled = true;

        // The variable codes are the DIMSTYLE table record's own group codes.
        switch (*variable) {
            case 3: if (text != nullptr) { style.linearPostfix = *text; } break;

            case 40: if (number) { style.overallScale = *number; } break;
            case 41: if (number) { style.arrowSize = *number; } break;
            case 42: if (number) { style.extensionLineOffset = *number; } break;
            case 43: if (number) { style.baselineSpacing = *number; } break;
            case 44: if (number) { style.extensionLineExtend = *number; } break;
            case 45: if (number) { style.roundOff = *number; } break;
            case 46: if (number) { style.dimensionLineExtend = *number; } break;
            case 49: if (number) { style.fixedExtensionLength = *number; } break;

            case 140: if (number) { style.textHeight = *number; } break;
            case 141: if (number) { style.centerMarkSize = *number; } break;
            case 144: if (number) { style.linearScale = *number; } break;
            case 147: if (number) { style.textGap = *number; } break;

            case 73: if (integer) { style.textInsideHorizontal = *integer != 0; } break;
            case 74: if (integer) { style.textOutsideHorizontal = *integer != 0; } break;
            case 75: if (integer) { style.suppressExtensionLine1 = *integer != 0; } break;
            case 76: if (integer) { style.suppressExtensionLine2 = *integer != 0; } break;
            case 77:
                if (integer && *integer >= 0 && *integer <= 4) {
                    style.textVerticalPosition = static_cast<TextVerticalPosition>(*integer);
                }
                break;
            case 78:
                if (integer) {
                    style.linearZeroSuppression =
                        ZeroSuppression::fromDimzin(static_cast<int>(*integer));
                }
                break;
            case 79:
                if (integer) {
                    style.angularZeroSuppression =
                        ZeroSuppression::fromDimzin(static_cast<int>(*integer));
                }
                break;

            case 173: if (integer) { style.separateArrowBlocks = *integer != 0; } break;
            case 176:
                if (integer) {
                    style.dimensionLineColor = Color::fromIndex(static_cast<int>(*integer));
                }
                break;
            case 177:
                if (integer) {
                    style.extensionLineColor = Color::fromIndex(static_cast<int>(*integer));
                }
                break;
            case 178:
                if (integer) { style.textColor = Color::fromIndex(static_cast<int>(*integer)); }
                break;
            case 179: if (integer) { style.angularPrecision = static_cast<int>(*integer); } break;

            case 271: if (integer) { style.linearPrecision = static_cast<int>(*integer); } break;
            case 275:
                if (integer && *integer >= 0 && *integer <= 4) {
                    style.angularFormat = static_cast<AngularUnitsFormat>(*integer);
                }
                break;
            case 277:
                if (integer && *integer >= 1 && *integer <= 6) {
                    style.linearFormat = static_cast<LinearUnitsFormat>(*integer);
                }
                break;
            case 278:
                if (integer && *integer > 0 && *integer < 128) {
                    style.decimalSeparator = static_cast<char>(*integer);
                }
                break;
            case 280:
                if (integer && *integer >= 0 && *integer <= 4) {
                    style.textHorizontalPosition = static_cast<TextHorizontalPosition>(*integer);
                }
                break;
            case 281: if (integer) { style.suppressDimensionLine1 = *integer != 0; } break;
            case 282: if (integer) { style.suppressDimensionLine2 = *integer != 0; } break;
            case 290: if (integer) { style.fixedExtensionLines = *integer != 0; } break;
            case 371:
                if (integer) {
                    style.dimensionLineWeight = static_cast<std::int16_t>(*integer);
                }
                break;
            case 372:
                if (integer) {
                    style.extensionLineWeight = static_cast<std::int16_t>(*integer);
                }
                break;

            case 340:
                if (const auto* handle = std::get_if<Handle>(&valueItem.value)) {
                    style.textStyleHandle = *handle;
                }
                break;

            default:
                // A variable this application does not model. Skipped rather
                // than guessed at, and the XData is untouched, so it still
                // round-trips to the file.
                handled = false;
                break;
        }
        if (handled) {
            ++applied;
        }
    }
    return applied;
}

// ----------------------------------------------------------------- formatting

std::string formatLinearMeasurement(const DimStyle& style, double distance) {
    if (!std::isfinite(distance)) {
        return applyPostfix(style.linearPostfix, "0");
    }

    double value = distance * style.linearScale;
    if (style.roundOff > 0.0) {
        value = std::round(value / style.roundOff) * style.roundOff;
    }
    // -0.0 prints as "-0", which looks like a defect in the drawing.
    if (value == 0.0) {
        value = 0.0;
    }

    std::string text;
    switch (style.linearFormat) {
        case LinearUnitsFormat::Scientific: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*E",
                          std::clamp(style.linearPrecision, 0, 16), value);
            text = buffer;
            break;
        }
        case LinearUnitsFormat::Engineering:
            text = feetAndInches(value, style, false);
            break;
        case LinearUnitsFormat::Architectural:
            text = feetAndInches(value, style, true);
            break;
        case LinearUnitsFormat::Fractional: {
            const bool negative = value < 0.0;
            text = mixedFraction(std::abs(value), style.linearPrecision, false);
            if (negative) { text = "-" + text; }
            break;
        }
        case LinearUnitsFormat::Decimal:
        case LinearUnitsFormat::WindowsDesktop:
        default:
            // WindowsDesktop means "follow the operating system's regional
            // settings". Deliberately treated as decimal: a drawing whose
            // dimensions read differently on the next machine is not a
            // drawing, and the separator is already a style setting.
            text = fixed(value, style.linearPrecision);
            applyZeroSuppression(text, style.linearZeroSuppression);
            break;
    }

    applySeparator(text, style.decimalSeparator);
    return applyPostfix(style.linearPostfix, text);
}

std::string formatAngularMeasurement(const DimStyle& style, double radians) {
    if (!std::isfinite(radians)) {
        return "0";
    }
    const int precision = std::clamp(style.angularPrecision, 0, 8);

    std::string text;
    switch (style.angularFormat) {
        case AngularUnitsFormat::DegreesMinutesSeconds: {
            double degrees = radToDeg(radians);
            const bool negative = degrees < 0.0;
            degrees = std::abs(degrees);

            auto whole = static_cast<long long>(std::floor(degrees));
            double minutes = (degrees - static_cast<double>(whole)) * 60.0;
            auto wholeMinutes = static_cast<long long>(std::floor(minutes));
            double seconds = (minutes - static_cast<double>(wholeMinutes)) * 60.0;

            // Carry, so 12deg 59' 60.0" reads as 13deg rather than as itself.
            std::string secondsText = fixed(seconds, precision);
            if (std::stod(secondsText) >= 60.0) {
                secondsText = fixed(0.0, precision);
                ++wholeMinutes;
            }
            if (wholeMinutes >= 60) {
                wholeMinutes -= 60;
                ++whole;
            }

            text = std::to_string(whole) + "\xC2\xB0" + std::to_string(wholeMinutes) + "'" +
                   secondsText + "\"";
            if (negative) { text = "-" + text; }
            break;
        }
        case AngularUnitsFormat::Gradians:
            text = fixed(radians * 200.0 / kPi, precision);
            applyZeroSuppression(text, style.angularZeroSuppression);
            text += 'g';
            break;
        case AngularUnitsFormat::Radians:
            text = fixed(radians, precision);
            applyZeroSuppression(text, style.angularZeroSuppression);
            text += 'r';
            break;
        case AngularUnitsFormat::SurveyorUnits: {
            // A bearing: two quadrant letters around an angle measured away
            // from the north-south axis, so never more than 90 degrees. The
            // four cardinal directions carry no angle at all.
            const double degrees = radToDeg(normalizeAngle(radians));
            const auto isNear = [degrees](double target) {
                return std::abs(degrees - target) < 1e-9;
            };
            if (isNear(0.0) || isNear(360.0)) { text = "E"; break; }
            if (isNear(90.0)) { text = "N"; break; }
            if (isNear(180.0)) { text = "W"; break; }
            if (isNear(270.0)) { text = "S"; break; }

            const char* meridian = nullptr;
            const char* side = nullptr;
            double bearing = 0.0;
            if (degrees < 90.0) {
                meridian = "N"; side = "E"; bearing = 90.0 - degrees;
            } else if (degrees < 180.0) {
                meridian = "N"; side = "W"; bearing = degrees - 90.0;
            } else if (degrees < 270.0) {
                meridian = "S"; side = "W"; bearing = 270.0 - degrees;
            } else {
                meridian = "S"; side = "E"; bearing = degrees - 270.0;
            }

            std::string bearingText = fixed(bearing, precision);
            applyZeroSuppression(bearingText, style.angularZeroSuppression);
            text = std::string{meridian} + " " + bearingText + "\xC2\xB0 " + side;
            break;
        }
        case AngularUnitsFormat::DecimalDegrees:
        default:
            text = fixed(radToDeg(radians), precision);
            applyZeroSuppression(text, style.angularZeroSuppression);
            text += "\xC2\xB0";  // U+00B0 DEGREE SIGN, in UTF-8.
            break;
    }
    return text;
}

}  // namespace cad
