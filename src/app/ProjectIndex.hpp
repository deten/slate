#pragma once

#include <functional>
#include <map>
#include <optional>

#include <QString>
#include <QStringList>

#include "cad/agent/AgentDialogue.hpp"
#include "cad/project/Project.hpp"

namespace cad::io {
class DwgTranslator;
}

namespace cad::app {

/// The project a drawing belongs to, and what its sheets hold.
///
/// A project file sits beside the drawings. When a drawing is opened, the
/// folder it is in is checked for one, and if found the set is known: which
/// files are sheets, in what order, what each is for, which backgrounds
/// stand behind them. Each sheet is read the way DESCRIBE reads the open
/// drawing, once, and the reading kept in a file beside the project until
/// the sheet changes. That is what the assistant is told about the set, and
/// what it can ask more about.
class ProjectIndex {
public:
    explicit ProjectIndex(const io::DwgTranslator* translator);

    /// The project file that governs `drawingPath`, if any: `project.slate`
    /// in the drawing's folder.
    [[nodiscard]] static QString projectFileFor(const QString& drawingPath);

    /// Use the project at `path`. False with the reason when it cannot be read.
    bool open(const QString& path, QString* error = nullptr);
    /// Use the project beside `drawingPath` if there is one; otherwise none.
    void openBeside(const QString& drawingPath);
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return !path_.isEmpty(); }
    [[nodiscard]] const QString& path() const noexcept { return path_; }
    [[nodiscard]] QString folder() const;
    [[nodiscard]] const Project& project() const noexcept { return project_; }
    [[nodiscard]] Project& project() noexcept { return project_; }

    /// Write the project back to its file. False with the reason on failure.
    bool save(QString* error = nullptr);

    /// The sheet a drawing file is listed as, if any.
    [[nodiscard]] const ProjectSheet* sheetForFile(const QString& drawingPath) const;

    /// A file's path as the project stores it: relative when inside the
    /// project folder, so the folder can move as a whole.
    [[nodiscard]] QString relativePath(const QString& absolutePath) const;
    [[nodiscard]] QString absolutePath(const QString& storedPath) const;

    /// The reading of one sheet -- what DESCRIBE would say of its file --
    /// from the cache when the file is unchanged, else read now. Empty when
    /// the file is missing or unreadable, with the trouble in `problems`.
    [[nodiscard]] QString readingOf(const ProjectSheet& sheet, QStringList* problems = nullptr);

    /// Every sheet in one line each, for the assistant: number, title,
    /// background, and the first line of what it holds.
    [[nodiscard]] QString digest(QStringList* problems = nullptr);

    /// Answer ASK SHEET <number> and ASK PROJECT; nothing for other kinds.
    [[nodiscard]] std::optional<std::string> answer(const AgentQuery& query);

    /// Read every sheet file now, cache aside, reporting progress by name.
    void refresh(const std::function<void(const QString&)>& progress = {},
                 QStringList* problems = nullptr);

    /// Make the sheet's file from the project's template: the template
    /// copied, its title block filled with the sheet's number and title and
    /// the project's name, written under the sheet's file name. The file
    /// must not exist yet. Returns the path written, or empty with `error`.
    [[nodiscard]] QString makeSheetFile(const ProjectSheet& sheet, QString* error);

    /// Make the file of every listed sheet that has none yet. Returns how
    /// many were made; the trouble with the rest goes to `problems`.
    int makeMissingSheetFiles(QStringList* made, QStringList* problems);

    /// Where a sibling sheet placed this background: its insertion point,
    /// scale and rotation in degrees, from the sheets' readings. False when
    /// no sheet in the set has it yet.
    struct Placement {
        double x = 0.0;
        double y = 0.0;
        double scale = 1.0;
        double rotationDegrees = 0.0;
        QString fromSheet;
    };
    [[nodiscard]] bool siblingPlacement(const ProjectBackground& background,
                                        const QString& exceptFile, Placement* placement);

    /// What is wrong with the set, from the list and the sheets' readings:
    /// a file missing, a number listed twice, a title block that says a
    /// different number or title than the list, a sheet listed with a
    /// background it does not reference, a background newer than a sheet
    /// that uses it. One line each; empty when all is well.
    [[nodiscard]] QStringList check();

private:
    struct Entry {
        QString key;
        QString reading;
    };
    [[nodiscard]] QString cachePath() const;
    void loadCache();
    void saveCache() const;
    [[nodiscard]] QString fileKey(const QString& absolutePath) const;
    [[nodiscard]] QString readSheetNow(const ProjectSheet& sheet, QString* problem);

    const io::DwgTranslator* translator_;
    QString path_;
    Project project_;
    /// Cached readings by stored file path.
    std::map<QString, Entry> cache_;
    bool cacheLoaded_ = false;
};

}  // namespace cad::app
