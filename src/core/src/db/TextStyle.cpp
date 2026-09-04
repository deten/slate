#include "cad/db/TextStyle.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace cad {

bool TextStyle::isTrueType() const {
    if (!typeface.empty()) {
        return true;
    }
    if (fontFile.size() < 4) {
        return false;
    }
    const std::string suffix = normalizeSymbolName(
        std::string_view{fontFile}.substr(fontFile.size() - 4));
    return suffix == ".TTF";
}

DbObjectPtr TextStyle::clone() const {
    auto copy = std::make_unique<TextStyle>();
    copyRecordInto(*copy);
    copy->fontFile = fontFile;
    copy->bigFontFile = bigFontFile;
    copy->typeface = typeface;
    copy->height = height;
    copy->widthFactor = widthFactor;
    copy->obliqueAngle = obliqueAngle;
    copy->isBackwards = isBackwards;
    copy->isUpsideDown = isUpsideDown;
    copy->isVertical = isVertical;
    copy->fontWeight = fontWeight;
    copy->isItalic = isItalic;
    copy->isShapeFile = isShapeFile;
    return copy;
}

void TextStyle::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const TextStyle*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a text style");
    }
    restoreRecordFrom(*source);
    fontFile = source->fontFile;
    bigFontFile = source->bigFontFile;
    typeface = source->typeface;
    height = source->height;
    widthFactor = source->widthFactor;
    obliqueAngle = source->obliqueAngle;
    isBackwards = source->isBackwards;
    isUpsideDown = source->isUpsideDown;
    isVertical = source->isVertical;
    fontWeight = source->fontWeight;
    isItalic = source->isItalic;
    isShapeFile = source->isShapeFile;
}

}  // namespace cad
