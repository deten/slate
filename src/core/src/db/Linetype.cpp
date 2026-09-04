#include "cad/db/Linetype.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace cad {
namespace {

// Pattern data for the standard linetypes, in drawing units at scale 1.
constexpr std::array<double, 0> kContinuous{};
constexpr std::array<double, 4> kCenter{31.75, -6.35, 6.35, -6.35};
constexpr std::array<double, 2> kDashed{12.7, -6.35};
constexpr std::array<double, 6> kPhantom{31.75, -6.35, 6.35, -6.35, 6.35, -6.35};
constexpr std::array<double, 4> kDashdot{12.7, -6.35, 0.0, -6.35};
constexpr std::array<double, 6> kDivide{12.7, -6.35, 0.0, -6.35, 0.0, -6.35};
constexpr std::array<double, 2> kDot{0.0, -6.35};
constexpr std::array<double, 2> kHidden{6.35, -3.175};

const std::array<StandardLinetype, 8> kStandardLinetypes{{
    {"Continuous", "Solid line", kContinuous},
    {"Center", "Center ____ _ ____ _ ____", kCenter},
    {"Dashed", "Dashed __ __ __ __ __ __", kDashed},
    {"Phantom", "Phantom ___ _ _ ___ _ _", kPhantom},
    {"Dashdot", "Dash dot __ . __ . __ .", kDashdot},
    {"Divide", "Divide __ . . __ . . __", kDivide},
    {"Dot", "Dot . . . . . . . . . .", kDot},
    {"Hidden", "Hidden __ __ __ __ __ __", kHidden},
}};

}  // namespace

bool Linetype::isReserved() const {
    const std::string upper = normalizeSymbolName(name);
    return upper == normalizeSymbolName(kLinetypeContinuous) ||
           upper == normalizeSymbolName(kLinetypeByLayer) ||
           upper == normalizeSymbolName(kLinetypeByBlock);
}

double Linetype::patternLength() const noexcept {
    double total = 0.0;
    for (const LinetypeElement& element : elements) {
        total += std::abs(element.length);
    }
    return total;
}

bool Linetype::isComplex() const noexcept {
    return std::any_of(elements.begin(), elements.end(), [](const LinetypeElement& element) {
        return !element.text.empty() || element.shapeNumber.has_value();
    });
}

std::vector<double> Linetype::toDashArray(double scale) const {
    if (isContinuous() || scale <= 0.0) {
        return {};
    }

    const double total = patternLength();
    std::vector<double> dashes;
    dashes.reserve(elements.size());

    for (const LinetypeElement& element : elements) {
        // A zero-length element is a dot. Rendering it as a true zero-length
        // dash makes it invisible in most rasterizers, so it is given a small
        // positive extent proportional to the pattern.
        const double length =
            element.length == 0.0 ? std::max(total * 0.01, 1e-6) : std::abs(element.length);
        dashes.push_back(length * scale);
    }

    // A dash array must have even length to alternate correctly; an odd
    // pattern is repeated so the on/off phases stay aligned.
    if (dashes.size() % 2 != 0) {
        const std::size_t original = dashes.size();
        dashes.reserve(original * 2);
        for (std::size_t i = 0; i < original; ++i) {
            dashes.push_back(dashes[i]);
        }
    }
    return dashes;
}

DbObjectPtr Linetype::clone() const {
    auto copy = std::make_unique<Linetype>();
    copyRecordInto(*copy);
    copy->description = description;
    copy->elements = elements;
    return copy;
}

void Linetype::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const Linetype*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a linetype");
    }
    restoreRecordFrom(*source);
    description = source->description;
    elements = source->elements;
}

std::vector<Handle> Linetype::referencedHandles() const {
    std::vector<Handle> refs = SymbolTableRecord::referencedHandles();
    for (const LinetypeElement& element : elements) {
        if (!element.styleHandle.isNull()) {
            refs.push_back(element.styleHandle);
        }
    }
    return refs;
}

std::span<const StandardLinetype> standardLinetypes() noexcept {
    return kStandardLinetypes;
}

}  // namespace cad
