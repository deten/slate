#include "ImageCache.hpp"

#include <QImageReader>
#include <algorithm>
#include <array>
#include <cmath>

namespace cad::app {
namespace {

/// other CAD software's brightness and contrast run 0-100 with 50 neutral.
///
/// Converted to the conventional offset-and-gain pair. The gain curve is the
/// usual one: it is monotonic, it leaves mid grey alone, and at the extremes it
/// approaches pure threshold rather than running away — which matters because a
/// scanned drawing pushed to contrast 100 is exactly how a drafter makes faint
/// pencil legible.
[[nodiscard]] QImage adjusted(const QImage& source, const ImageCache::Adjustment& adjustment) {
    if (adjustment.isNeutral()) {
        return source;
    }

    const double offset = (adjustment.brightness - 50) * 2.55;
    const double contrast = (adjustment.contrast - 50) * 2.55;
    const double gain = (259.0 * (contrast + 255.0)) / (255.0 * (259.0 - contrast));

    // A lookup table, because the alternative is this arithmetic 34 million
    // times on the sample drawing's scan.
    std::array<int, 256> table{};
    for (int i = 0; i < 256; ++i) {
        const double value = gain * ((i + offset) - 128.0) + 128.0;
        table[static_cast<std::size_t>(i)] = std::clamp(static_cast<int>(std::lround(value)), 0,
                                                        255);
    }

    QImage result = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < result.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const QRgb pixel = line[x];
            line[x] = qRgba(table[static_cast<std::size_t>(qRed(pixel))],
                            table[static_cast<std::size_t>(qGreen(pixel))],
                            table[static_cast<std::size_t>(qBlue(pixel))], qAlpha(pixel));
        }
    }
    return result;
}

}  // namespace

int ImageCache::levelFor(const QSize& full, int targetWidth, int targetHeight) {
    if (full.isEmpty() || targetWidth <= 0 || targetHeight <= 0) {
        return 0;
    }
    // The smaller of the two ratios, so neither axis is ever decoded coarser
    // than it is drawn — a clipped image whose visible sliver is tall and
    // narrow still gets the resolution its height demands.
    const double ratio = std::min(static_cast<double>(full.width()) / targetWidth,
                                  static_cast<double>(full.height()) / targetHeight);
    if (ratio <= 1.0) {
        return 0;
    }
    // Round down to a power of two: level 1 is half size, level 2 a quarter.
    // Capped so a pathological zoom-out cannot reduce an image to nothing.
    return std::clamp(static_cast<int>(std::floor(std::log2(ratio))), 0, 6);
}

const QImage* ImageCache::image(const QString& path, int targetWidth, int targetHeight,
                                const Adjustment& adjustment) {
    if (path.isEmpty() || failed_.contains(path)) {
        return nullptr;
    }

    QImageReader reader{path};
    const QSize full = reader.size();
    if (!full.isValid() || full.isEmpty()) {
        failed_.insert(path, true);
        return nullptr;
    }

    const int level = levelFor(full, targetWidth, targetHeight);
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(path)
                            .arg(level)
                            .arg(adjustment.brightness)
                            .arg(adjustment.contrast);

    if (const auto it = index_.find(key); it != index_.end()) {
        // Move to the front: the eviction order is the whole point of the list.
        entries_.splice(entries_.begin(), entries_, it.value());
        it.value() = entries_.begin();
        return &entries_.front().image;
    }

    if (level > 0) {
        reader.setScaledSize(QSize{std::max(1, full.width() >> level),
                                   std::max(1, full.height() >> level)});
    }

    QImage decoded = reader.read();
    if (decoded.isNull()) {
        failed_.insert(path, true);
        return nullptr;
    }
    decoded = adjusted(decoded, adjustment);

    Entry entry;
    entry.bytes = static_cast<std::size_t>(decoded.sizeInBytes());
    entry.image = std::move(decoded);
    entry.key = key;

    // Evict before inserting, and leave room for what is about to arrive, so
    // that a single image larger than the budget still displays rather than
    // evicting itself the moment it lands.
    evictTo(entry.bytes < budgetBytes_ ? budgetBytes_ - entry.bytes : 0);

    bytes_ += entry.bytes;
    entries_.push_front(std::move(entry));
    index_.insert(key, entries_.begin());
    return &entries_.front().image;
}

void ImageCache::evictTo(std::size_t budget) {
    while (bytes_ > budget && !entries_.empty()) {
        const Entry& oldest = entries_.back();
        bytes_ -= oldest.bytes;
        index_.remove(oldest.key);
        entries_.pop_back();
    }
}

void ImageCache::clear() {
    entries_.clear();
    index_.clear();
    failed_.clear();
    bytes_ = 0;
}

}  // namespace cad::app
