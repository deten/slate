#pragma once

#include <functional>

#include <QString>
#include <QStringList>

namespace cad::io {
class DwgTranslator;
}

namespace cad::app {

/// The folder of drawings the assistant learns the office's habits from.
///
/// One drawing cannot tell an assistant which layer a wall goes on or what the
/// door block is called. The last dozen drawings the office produced can. The
/// user points at a folder; the drawings in it are read once and boiled down
/// to the conventions they share, and any notes left beside them (a text or
/// markdown file saying what the layers are for, say) are passed along as
/// written. The result goes into every request.
///
/// Reading a folder of DWGs takes a while, so the result is kept on disk and
/// reused until a file in the folder changes.
class ReferenceLibrary {
public:
    ReferenceLibrary(const io::DwgTranslator* translator, QString cacheDirectory);

    struct Files {
        QStringList drawings;
        QStringList notes;
        int skipped = 0;
    };

    struct Result {
        QString text;
        int drawings = 0;
        int notes = 0;
        bool fromCache = false;
        QStringList problems;
        [[nodiscard]] bool empty() const { return text.isEmpty(); }
    };

    /// The drawings and notes a folder holds, deepest folders included.
    [[nodiscard]] static Files filesIn(const QString& directory);

    /// What the folder teaches, read fresh only when its files changed.
    [[nodiscard]] Result read(const QString& directory,
                              const std::function<void(const QString&)>& progress = {});

private:
    [[nodiscard]] QString cachePath() const;

    const io::DwgTranslator* translator_;
    QString cacheDirectory_;
};

}  // namespace cad::app
