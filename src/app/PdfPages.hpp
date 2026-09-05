#pragma once

#include <cstddef>
#include <optional>

#include <QSizeF>
#include <QString>

namespace cad {
class Database;
}

namespace cad::app {

/// How many pages a PDF has, or zero when it cannot be opened.
[[nodiscard]] int pdfPageCount(const QString& path);

/// A page's size in points, 72 to the inch. Pages count from one, as the
/// drawing file counts them. Nothing when the file or page is not there.
[[nodiscard]] std::optional<QSizeF> pdfPageSizePoints(const QString& path, int page);

/// Give every PDF underlay whose page size is not yet known the size of its
/// page in drawing units, read from the file it resolved to.
///
/// The drawing stores where a page goes and which page it is, not how big the
/// page is; only the PDF knows that. Until it is known the underlay has no
/// outline and cannot be drawn. Returns how many were filled in.
std::size_t fillUnderlayPageSizes(Database& db);

}  // namespace cad::app
