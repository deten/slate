#pragma once

#include <QHash>
#include <QImage>
#include <QString>
#include <cstddef>
#include <list>

namespace cad::app {

/// Decoded raster images, kept between frames.
///
/// A drawing's attached images are large — the scan in the sample drawing is
/// 7177 x 4833 pixels, 132 megabytes decoded — and a viewport that redraws on
/// every mouse move cannot decode one per frame. Two things follow from that,
/// and both are why this exists rather than a call to `QImage::load`:
///
///  1. **Decode at the size actually needed.** `QImageReader::setScaledSize`
///     scales during decoding, so an image shown 400 pixels wide never
///     allocates 132 megabytes at all. Zooming in re-decodes at a finer level,
///     which is the moment the detail becomes worth paying for.
///  2. **Bound the memory.** A drawing set with thirty attached scans would
///     otherwise decode all thirty and stay that way. Least-recently-used
///     entries are dropped once the budget is passed.
///
/// Brightness, contrast and fade are baked into the cached copy rather than
/// applied per frame: they are per-*placement* settings that almost never
/// change, and a per-pixel pass over a megapixel image on every repaint would
/// cost more than the decode it is trying to avoid.
class ImageCache {
public:
    /// How the image should look, from the IMAGE entity's display settings.
    struct Adjustment {
        int brightness = 50;
        int contrast = 50;

        [[nodiscard]] bool isNeutral() const noexcept {
            return brightness == 50 && contrast == 50;
        }
        [[nodiscard]] bool operator==(const Adjustment&) const noexcept = default;
    };

    explicit ImageCache(std::size_t budgetBytes = 256u * 1024u * 1024u)
        : budgetBytes_(budgetBytes) {}

    /// An image suitable for drawing at roughly `targetWidth` by
    /// `targetHeight` device pixels.
    ///
    /// Null when the file cannot be read or decoded, which the caller must draw
    /// a placeholder for — the alternative is a blank rectangle indisguishable
    /// from an image that legitimately contains nothing.
    [[nodiscard]] const QImage* image(const QString& path, int targetWidth, int targetHeight,
                                      const Adjustment& adjustment = {});

    void clear();

    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

    /// Files that were tried and could not be decoded, so the failure is
    /// reported once rather than retried on every frame.
    [[nodiscard]] bool hasFailed(const QString& path) const { return failed_.contains(path); }

private:
    struct Entry {
        QImage image;
        std::size_t bytes = 0;
        QString key;
    };

    /// Which power-of-two reduction serves a target size.
    ///
    /// Quantised rather than exact, so that panning and small zoom changes
    /// reuse the decode instead of throwing it away. Zooming past a factor of
    /// two is what earns a new one.
    [[nodiscard]] static int levelFor(const QSize& full, int targetWidth, int targetHeight);

    void evictTo(std::size_t budget);

    std::size_t budgetBytes_;
    std::size_t bytes_ = 0;
    /// Most recently used first.
    std::list<Entry> entries_;
    QHash<QString, std::list<Entry>::iterator> index_;
    QHash<QString, bool> failed_;
};

}  // namespace cad::app
