#include "ProjectIndex.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

#include "cad/agent/DrawingReading.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/io/DxfWriter.hpp"
#include "cad/io/XrefLoader.hpp"

namespace cad::app {
namespace {

/// The cache file's record separator: a line no reading contains.
const QString kRecord = QStringLiteral("=== sheet ");

}  // namespace

ProjectIndex::ProjectIndex(const io::DwgTranslator* translator) : translator_(translator) {}

QString ProjectIndex::projectFileFor(const QString& drawingPath) {
    if (drawingPath.isEmpty()) {
        return {};
    }
    const QString candidate =
        QFileInfo{drawingPath}.dir().filePath(QString::fromUtf8(kProjectFileName));
    return QFileInfo::exists(candidate) ? candidate : QString{};
}

bool ProjectIndex::open(const QString& path, QString* error) {
    std::string problem;
    Project project = loadProject(std::filesystem::path{path.toStdWString()}, &problem);
    if (!problem.empty()) {
        if (error != nullptr) {
            *error = QString::fromStdString(problem);
        }
        return false;
    }
    close();
    path_ = QFileInfo{path}.absoluteFilePath();
    project_ = std::move(project);
    project_.sortSheets();
    return true;
}

void ProjectIndex::openBeside(const QString& drawingPath) {
    const QString file = projectFileFor(drawingPath);
    if (file.isEmpty()) {
        close();
        return;
    }
    if (file != path_) {
        open(file);
    }
}

void ProjectIndex::close() {
    path_.clear();
    project_ = Project{};
    cache_.clear();
    cacheLoaded_ = false;
}

QString ProjectIndex::folder() const {
    return path_.isEmpty() ? QString{} : QFileInfo{path_}.absolutePath();
}

bool ProjectIndex::save(QString* error) {
    if (path_.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("No project is open.");
        }
        return false;
    }
    project_.sortSheets();
    std::string problem;
    if (!saveProject(project_, std::filesystem::path{path_.toStdWString()}, &problem)) {
        if (error != nullptr) {
            *error = QString::fromStdString(problem);
        }
        return false;
    }
    return true;
}

const ProjectSheet* ProjectIndex::sheetForFile(const QString& drawingPath) const {
    if (drawingPath.isEmpty() || path_.isEmpty()) {
        return nullptr;
    }
    const QString wanted = QFileInfo{drawingPath}.absoluteFilePath();
    for (const ProjectSheet& sheet : project_.sheets) {
        if (QFileInfo{absolutePath(QString::fromStdString(sheet.file))}.absoluteFilePath() ==
            wanted) {
            return &sheet;
        }
    }
    return nullptr;
}

QString ProjectIndex::relativePath(const QString& absolute) const {
    const QDir base{folder()};
    const QString relative = base.relativeFilePath(absolute);
    // Outside the folder the relative form climbs out with ".."; keep the
    // absolute path then, since a set's own files are what move together.
    return relative.startsWith(QStringLiteral("..")) ? absolute : relative;
}

QString ProjectIndex::absolutePath(const QString& stored) const {
    if (stored.isEmpty()) {
        return {};
    }
    const QFileInfo info{stored};
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }
    return QDir{folder()}.absoluteFilePath(stored);
}

QString ProjectIndex::cachePath() const {
    return path_.isEmpty() ? QString{} : path_ + QStringLiteral(".index");
}

QString ProjectIndex::fileKey(const QString& absolute) const {
    const QFileInfo info{absolute};
    if (!info.exists()) {
        return {};
    }
    return QString::number(info.size()) + QLatin1Char('|') +
           QString::number(info.lastModified().toMSecsSinceEpoch());
}

void ProjectIndex::loadCache() {
    if (cacheLoaded_) {
        return;
    }
    cacheLoaded_ = true;
    cache_.clear();
    QFile file{cachePath()};
    if (cachePath().isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    // Records: "=== sheet <stored path> | <key>" then the reading until the
    // next record.
    QString current;
    Entry entry;
    const QString all = QString::fromUtf8(file.readAll());
    for (const QString& line : all.split(QLatin1Char('\n'))) {
        if (line.startsWith(kRecord)) {
            if (!current.isEmpty()) {
                cache_[current] = entry;
            }
            const QString head = line.mid(kRecord.size());
            const int bar = head.lastIndexOf(QStringLiteral(" | "));
            current = bar < 0 ? head : head.left(bar);
            entry = Entry{};
            entry.key = bar < 0 ? QString{} : head.mid(bar + 3);
            continue;
        }
        if (!current.isEmpty()) {
            entry.reading += line + QLatin1Char('\n');
        }
    }
    if (!current.isEmpty()) {
        cache_[current] = entry;
    }
    for (auto& [stored, cached] : cache_) {
        cached.reading = cached.reading.trimmed();
    }
}

void ProjectIndex::saveCache() const {
    QFile file{cachePath()};
    if (cachePath().isEmpty() || !file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    QTextStream out{&file};
    out << "# What each sheet of the project holds, read by Slate. Rebuilt as sheets change.\n";
    for (const auto& [stored, entry] : cache_) {
        out << kRecord << stored << " | " << entry.key << '\n' << entry.reading << '\n';
    }
}

QString ProjectIndex::readSheetNow(const ProjectSheet& sheet, QString* problem) {
    const QString absolute = absolutePath(QString::fromStdString(sheet.file));
    if (absolute.isEmpty() || !QFileInfo::exists(absolute)) {
        if (problem != nullptr) {
            *problem = QStringLiteral("%1: file not found (%2)")
                           .arg(QString::fromStdString(sheet.number),
                                QString::fromStdString(sheet.file));
        }
        return {};
    }
    try {
        io::XrefLoadOptions options;
        options.translator = translator_;
        // Backgrounds are read too: the rooms of a plan sheet are usually in
        // the architecture behind it.
        options.maxDepth = 1;
        io::DrawingLoadResult loaded =
            io::loadDrawingWithReferences(std::filesystem::path{absolute.toStdWString()}, options);
        if (loaded.database == nullptr) {
            if (problem != nullptr) {
                *problem = QStringLiteral("%1: could not be read")
                               .arg(QString::fromStdString(sheet.number));
            }
            return {};
        }
        const DrawingReading reading = readDrawing(*loaded.database);
        QString text = QString::fromStdString(reading.describe()).trimmed();
        return text.isEmpty() ? QStringLiteral("Nothing was read from it.") : text;
    } catch (const std::exception& error) {
        if (problem != nullptr) {
            *problem = QStringLiteral("%1: %2")
                           .arg(QString::fromStdString(sheet.number),
                                QString::fromUtf8(error.what()));
        }
        return {};
    }
}

QString ProjectIndex::readingOf(const ProjectSheet& sheet, QStringList* problems) {
    loadCache();
    const QString stored = QString::fromStdString(sheet.file);
    const QString key = fileKey(absolutePath(stored));
    if (const auto found = cache_.find(stored);
        found != cache_.end() && !key.isEmpty() && found->second.key == key) {
        return found->second.reading;
    }
    QString problem;
    const QString reading = readSheetNow(sheet, &problem);
    if (!problem.isEmpty() && problems != nullptr) {
        *problems << problem;
    }
    if (!reading.isEmpty()) {
        cache_[stored] = Entry{key, reading};
        saveCache();
    }
    return reading;
}

void ProjectIndex::refresh(const std::function<void(const QString&)>& progress,
                           QStringList* problems) {
    loadCache();
    cache_.clear();
    for (const ProjectSheet& sheet : project_.sheets) {
        if (progress) {
            progress(QString::fromStdString(sheet.number));
        }
        static_cast<void>(readingOf(sheet, problems));
    }
}

QString ProjectIndex::digest(QStringList* problems) {
    if (!isOpen()) {
        return {};
    }
    QString text = QStringLiteral("Project: %1").arg(QString::fromStdString(project_.name));
    if (!project_.discipline.empty()) {
        text += QStringLiteral(" (%1)").arg(QString::fromStdString(project_.discipline));
    }
    text += QStringLiteral("\nSheets (%1):\n").arg(project_.sheets.size());
    for (const ProjectSheet& sheet : project_.sheets) {
        text += QStringLiteral("  %1 %2").arg(QString::fromStdString(sheet.number),
                                              QString::fromStdString(sheet.title));
        if (!sheet.background.empty()) {
            text += QStringLiteral(", background %1").arg(QString::fromStdString(sheet.background));
        }
        if (!sheet.status.empty()) {
            text += QStringLiteral(", %1").arg(QString::fromStdString(sheet.status));
        }
        // One line of what it holds: the room count and the system layers,
        // which is what tells a model which sheet a question is about.
        const QString reading = readingOf(sheet, problems);
        QStringList gist;
        for (const QString& line : reading.split(QLatin1Char('\n'))) {
            if (line.startsWith(QStringLiteral("Rooms ("))) {
                // "Rooms (40, 3 with walls traced):" -> "40 rooms".
                const int open = line.indexOf(QLatin1Char('('));
                int end = line.indexOf(QLatin1Char(','), open);
                if (end < 0) {
                    end = line.indexOf(QLatin1Char(')'), open);
                }
                gist << line.mid(open + 1, end - open - 1).trimmed() + QStringLiteral(" rooms");
            } else if (line.startsWith(QStringLiteral("  ")) &&
                       (line.contains(QStringLiteral("duct")) ||
                        line.contains(QStringLiteral("(pipe")) ||
                        line.contains(QStringLiteral("(equipment")))) {
                // "  MDUCT-D (duct, demolition): ..." -> the layer and its kind.
                const QString item = line.trimmed();
                gist << item.left(item.indexOf(QStringLiteral("):")) + 1);
            }
        }
        if (reading.isEmpty()) {
            text += QStringLiteral(" - not read");
        } else if (!gist.isEmpty()) {
            text += QStringLiteral(" - ") + gist.mid(0, 4).join(QStringLiteral("; "));
        }
        text += QLatin1Char('\n');
    }
    if (!project_.backgrounds.empty()) {
        text += QStringLiteral("Backgrounds:");
        for (const ProjectBackground& background : project_.backgrounds) {
            text += QStringLiteral(" %1 = %2;").arg(QString::fromStdString(background.name),
                                                   QString::fromStdString(background.file));
        }
        text += QLatin1Char('\n');
    }
    if (!project_.notes.empty()) {
        text += QStringLiteral("Project notes: %1\n").arg(QString::fromStdString(project_.notes));
    }
    text += QStringLiteral("Ask SHEET <number> for what a sheet holds.\n");
    return text;
}

QString ProjectIndex::makeSheetFile(const ProjectSheet& sheet, QString* error) {
    const auto fail = [&](const QString& why) {
        if (error != nullptr) {
            *error = QStringLiteral("%1: %2").arg(QString::fromStdString(sheet.number), why);
        }
        return QString{};
    };
    if (!isOpen()) {
        return fail(QStringLiteral("no project is open"));
    }
    if (project_.sheetTemplate.empty()) {
        return fail(QStringLiteral("the project has no sheet template"));
    }
    const QString templatePath = absolutePath(QString::fromStdString(project_.sheetTemplate));
    if (!QFileInfo::exists(templatePath)) {
        return fail(QStringLiteral("template not found (%1)").arg(templatePath));
    }
    // The file name follows the sheet: what the list says, or the number
    // with the template's extension.
    QString stored = QString::fromStdString(sheet.file);
    if (stored.isEmpty()) {
        stored = QString::fromStdString(sheet.number) + QLatin1Char('.') +
                 QFileInfo{templatePath}.suffix();
    }
    const QString target = absolutePath(stored);
    if (QFileInfo::exists(target)) {
        return fail(QStringLiteral("%1 already exists").arg(stored));
    }

    try {
        io::XrefLoadOptions options;
        options.translator = translator_;
        options.maxDepth = 0;
        io::DrawingLoadResult loaded = io::loadDrawingWithReferences(
            std::filesystem::path{templatePath.toStdWString()}, options);
        if (loaded.database == nullptr) {
            return fail(QStringLiteral("the template could not be read"));
        }
        Database& db = *loaded.database;

        // The title block's fields, by the tags offices use. A tag that
        // says SHEET gets the number; one that says TITLE gets the title,
        // split over LINE1/LINE2/... when the block has several; PROJECT
        // or JOB gets the project's name. Every other field is left as the
        // template has it.
        const QStringList titleWords = QString::fromStdString(sheet.title).split(
            QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const Handle handle : db.objectHandles()) {
            auto* attribute = db.get<AttributeEntity>(handle);
            if (attribute == nullptr || attribute->erased) {
                continue;
            }
            const QString tag = QString::fromStdString(attribute->tag).toUpper();
            if (tag.contains(QStringLiteral("TITLE"))) {
                // "SHEETTITLE-LINE2(20LETTERS)": the line's number, if any.
                int line = 1;
                int lines = 1;
                const int at = tag.indexOf(QStringLiteral("LINE"));
                if (at >= 0) {
                    line = std::max(1, tag.mid(at + 4, 1).toInt());
                    lines = 3;
                }
                if (lines == 1) {
                    attribute->setValue(sheet.title);
                } else {
                    // Spread the words across the lines, filling in order.
                    const int words = static_cast<int>(titleWords.size());
                    const int perLine = std::max(1, (words + lines - 1) / lines);
                    const QStringList mine = titleWords.mid((line - 1) * perLine, perLine);
                    attribute->setValue(mine.join(QLatin1Char(' ')).toStdString());
                }
            } else if (tag.contains(QStringLiteral("SHEET")) || tag == QStringLiteral("DWGNO") ||
                       tag == QStringLiteral("NUMBER") || tag == QStringLiteral("SHT")) {
                attribute->setValue(sheet.number);
            } else if (tag.contains(QStringLiteral("PROJECT")) || tag.contains(QStringLiteral("JOB"))) {
                if (!project_.name.empty()) {
                    attribute->setValue(project_.name);
                }
            }
        }

        io::DxfWriter writer;
        const std::string text = writer.write(db);
        const std::filesystem::path path{target.toStdWString()};
        if (target.endsWith(QStringLiteral(".dwg"), Qt::CaseInsensitive)) {
            if (translator_ == nullptr || !translator_->canWriteDwg()) {
                return fail(QStringLiteral("no DWG translator is available; use a DXF template"));
            }
            translator_->writeDwg(text, path);
        } else {
            QFile file{target};
            if (!file.open(QIODevice::WriteOnly)) {
                return fail(QStringLiteral("cannot create %1").arg(target));
            }
            file.write(text.data(), static_cast<qint64>(text.size()));
        }
    } catch (const std::exception& problem) {
        return fail(QString::fromUtf8(problem.what()));
    }

    // The list learns the file name it now has.
    if (ProjectSheet* listed = project_.findSheet(sheet.number); listed != nullptr && listed->file.empty()) {
        listed->file = stored.toStdString();
        save();
    }
    return target;
}

int ProjectIndex::makeMissingSheetFiles(QStringList* made, QStringList* problems) {
    int count = 0;
    // Over a copy: making a file may write the list.
    const std::vector<ProjectSheet> sheets = project_.sheets;
    for (const ProjectSheet& sheet : sheets) {
        const QString stored = QString::fromStdString(sheet.file);
        if (!stored.isEmpty() && QFileInfo::exists(absolutePath(stored))) {
            continue;
        }
        QString error;
        const QString path = makeSheetFile(sheet, &error);
        if (path.isEmpty()) {
            if (problems != nullptr) {
                *problems << error;
            }
            continue;
        }
        ++count;
        if (made != nullptr) {
            *made << path;
        }
    }
    return count;
}

bool ProjectIndex::siblingPlacement(const ProjectBackground& background,
                                    const QString& exceptFile, Placement* placement) {
    // A reference is named by its file's stem, and a sheet's reading lists
    // its references as "NAME at x,y scale s rotation r".
    const QString stem = QFileInfo{QString::fromStdString(background.file)}.completeBaseName();
    if (stem.isEmpty()) {
        return false;
    }
    const QString except = exceptFile.isEmpty() ? QString{} : QFileInfo{exceptFile}.absoluteFilePath();
    for (const ProjectSheet& sheet : project_.sheets) {
        if (sheet.background != background.name) {
            continue;
        }
        const QString absolute = absolutePath(QString::fromStdString(sheet.file));
        if (absolute.isEmpty() || QFileInfo{absolute}.absoluteFilePath() == except) {
            continue;
        }
        const QString reading = readingOf(sheet);
        for (const QString& line : reading.split(QLatin1Char('\n'))) {
            if (!line.startsWith(QStringLiteral("References:"))) {
                continue;
            }
            for (const QString& item : line.mid(11).split(QLatin1Char(';'))) {
                const QStringList words = item.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
                // NAME at x,y scale s rotation r [(not found)]
                if (words.size() < 7 || words[0].compare(stem, Qt::CaseInsensitive) != 0) {
                    continue;
                }
                const QStringList xy = words[2].split(QLatin1Char(','));
                if (xy.size() != 2) {
                    continue;
                }
                if (placement != nullptr) {
                    placement->x = xy[0].toDouble();
                    placement->y = xy[1].toDouble();
                    placement->scale = words[4].toDouble();
                    placement->rotationDegrees = words[6].toDouble();
                    placement->fromSheet = QString::fromStdString(sheet.number);
                }
                return true;
            }
        }
    }
    return false;
}

std::optional<std::string> ProjectIndex::answer(const AgentQuery& query) {
    if (query.kind != "SHEET" && query.kind != "PROJECT") {
        return std::nullopt;
    }
    if (!isOpen()) {
        return std::string{"No project is open beside this drawing.\n"};
    }
    if (query.kind == "PROJECT") {
        return digest().toStdString();
    }
    const QString wanted = QString::fromStdString(query.argument).trimmed();
    const ProjectSheet* sheet = project_.findSheet(wanted.toStdString());
    if (sheet == nullptr) {
        for (const ProjectSheet& candidate : project_.sheets) {
            if (QString::fromStdString(candidate.number).compare(wanted, Qt::CaseInsensitive) == 0) {
                sheet = &candidate;
            }
        }
    }
    if (sheet == nullptr) {
        QString text = QStringLiteral("No sheet %1. Sheets are:").arg(wanted);
        for (const ProjectSheet& candidate : project_.sheets) {
            text += QStringLiteral(" %1;").arg(QString::fromStdString(candidate.number));
        }
        return (text + QLatin1Char('\n')).toStdString();
    }
    QStringList problems;
    const QString reading = readingOf(*sheet, &problems);
    QString text = QStringLiteral("Sheet %1 %2 (%3)")
                       .arg(QString::fromStdString(sheet->number),
                            QString::fromStdString(sheet->title),
                            QString::fromStdString(sheet->file));
    if (!sheet->background.empty()) {
        text += QStringLiteral(", background %1").arg(QString::fromStdString(sheet->background));
    }
    text += QLatin1Char('\n');
    text += reading.isEmpty() ? problems.join(QLatin1Char('\n')) : reading;
    return (text + QLatin1Char('\n')).toStdString();
}

}  // namespace cad::app
