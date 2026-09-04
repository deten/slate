#include "ReferenceLibrary.hpp"

#include <exception>
#include <filesystem>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include "cad/agent/DrawingStandards.hpp"
#include "cad/io/XrefLoader.hpp"

namespace cad::app {
namespace {

constexpr int kMaxDrawings = 100;
constexpr int kMaxNoteChars = 4000;
constexpr int kMaxNotesTotal = 12000;
constexpr const char* kCacheName = "reference-standards.txt";
/// Bumped when the text this produces changes shape, so an old cache is not
/// mistaken for a current one.
constexpr const char* kCacheFormat = "1";

/// Folders that hold copies rather than work: recycle bins, thumbnail stores
/// and anything hidden. A deleted drawing in the recycle bin is not a
/// convention.
[[nodiscard]] bool isSideFolder(const QString& relativePath) {
    for (const QString& part : relativePath.split('/', Qt::SkipEmptyParts)) {
        if (part.startsWith('.') || part.startsWith('#') || part.startsWith('@') ||
            part.startsWith('~')) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString signatureOf(const ReferenceLibrary::Files& files) {
    QCryptographicHash hash{QCryptographicHash::Sha1};
    hash.addData(QByteArray{kCacheFormat});
    for (const QString& path : files.drawings + files.notes) {
        const QFileInfo info{path};
        hash.addData((path + '|' + QString::number(info.size()) + '|' +
                      QString::number(info.lastModified().toMSecsSinceEpoch()) + '\n')
                         .toUtf8());
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString readNote(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QString text = QString::fromUtf8(file.readAll()).trimmed();
    if (text.size() > kMaxNoteChars) {
        text = text.left(kMaxNoteChars) + QStringLiteral("\n[cut here]");
    }
    return text;
}

}  // namespace

ReferenceLibrary::ReferenceLibrary(const io::DwgTranslator* translator, QString cacheDirectory)
    : translator_(translator), cacheDirectory_(std::move(cacheDirectory)) {}

ReferenceLibrary::Files ReferenceLibrary::filesIn(const QString& directory) {
    Files files;
    const QDir root{directory};
    if (directory.isEmpty() || !root.exists()) {
        return files;
    }
    QDirIterator it{directory, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories};
    while (it.hasNext()) {
        const QString path = it.next();
        if (isSideFolder(root.relativeFilePath(path))) {
            continue;
        }
        const QString suffix = QFileInfo{path}.suffix().toLower();
        if (suffix == QLatin1String("dwg") || suffix == QLatin1String("dxf")) {
            files.drawings << path;
        } else if (suffix == QLatin1String("txt") || suffix == QLatin1String("md")) {
            files.notes << path;
        }
    }
    files.drawings.sort(Qt::CaseInsensitive);
    files.notes.sort(Qt::CaseInsensitive);
    if (files.drawings.size() > kMaxDrawings) {
        files.skipped = static_cast<int>(files.drawings.size()) - kMaxDrawings;
        files.drawings = files.drawings.mid(0, kMaxDrawings);
    }
    return files;
}

QString ReferenceLibrary::cachePath() const {
    return QDir{cacheDirectory_}.filePath(QString::fromLatin1(kCacheName));
}

ReferenceLibrary::Result ReferenceLibrary::read(
    const QString& directory, const std::function<void(const QString&)>& progress) {
    Result result;
    if (directory.trimmed().isEmpty()) {
        return result;
    }
    if (!QDir{directory}.exists()) {
        result.problems << QStringLiteral("The reference folder %1 does not exist.")
                               .arg(QDir::toNativeSeparators(directory));
        return result;
    }

    const Files files = filesIn(directory);
    result.drawings = static_cast<int>(files.drawings.size());
    result.notes = static_cast<int>(files.notes.size());
    if (files.drawings.isEmpty() && files.notes.isEmpty()) {
        result.problems << QStringLiteral("No drawings or notes were found in %1.")
                               .arg(QDir::toNativeSeparators(directory));
        return result;
    }
    if (files.skipped > 0) {
        result.problems << QStringLiteral("Only the first %1 drawings are read; %2 were left out.")
                               .arg(kMaxDrawings)
                               .arg(files.skipped);
    }

    // The cache is keyed on the folder and on every file's size and time, so a
    // changed drawing is re-read and an unchanged folder costs nothing.
    const QString signature = signatureOf(files);
    const QString header =
        QStringLiteral("signature %1\nfolder %2\n").arg(signature, QDir::cleanPath(directory));
    if (QFile cache{cachePath()}; cache.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString stored = QString::fromUtf8(cache.readAll());
        if (stored.startsWith(header)) {
            result.text = stored.mid(header.size());
            result.fromCache = true;
            return result;
        }
    }

    DrawingStandards standards;
    io::XrefLoadOptions options;
    options.translator = translator_;
    // The drawing's own contents are the convention; what it references is
    // somebody else's, and following it would double the reading time.
    options.maxDepth = 0;
    for (const QString& path : files.drawings) {
        const QString name = QFileInfo{path}.fileName();
        if (progress) {
            progress(name);
        }
        try {
            io::DrawingLoadResult loaded =
                io::loadDrawingWithReferences(std::filesystem::path{path.toStdWString()}, options);
            if (loaded.database == nullptr) {
                result.problems << QStringLiteral("%1 could not be read.").arg(name);
                continue;
            }
            standards.learnFrom(*loaded.database, name.toStdString());
        } catch (const std::exception& error) {
            result.problems << QStringLiteral("%1: %2").arg(name, QString::fromUtf8(error.what()));
        }
    }

    QString text = QStringLiteral("Reference folder: %1\n").arg(QDir::toNativeSeparators(directory));
    text += QString::fromStdString(standards.describe());

    int noteChars = 0;
    for (const QString& path : files.notes) {
        const QString note = readNote(path);
        if (note.isEmpty()) {
            continue;
        }
        if (noteChars + note.size() > kMaxNotesTotal) {
            result.problems << QStringLiteral("Notes beyond %1 characters were left out, from %2 on.")
                                   .arg(kMaxNotesTotal)
                                   .arg(QFileInfo{path}.fileName());
            break;
        }
        noteChars += static_cast<int>(note.size());
        text += QStringLiteral("\nNotes from %1:\n%2\n").arg(QFileInfo{path}.fileName(), note);
    }
    result.text = text;

    if (QDir{}.mkpath(cacheDirectory_)) {
        if (QFile cache{cachePath()}; cache.open(QIODevice::WriteOnly | QIODevice::Text)) {
            cache.write((header + text).toUtf8());
        }
    }
    return result;
}

}  // namespace cad::app
