#include "ProjectDialog.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "ProjectIndex.hpp"
#include "cad/agent/DrawingReading.hpp"
#include "cad/io/XrefLoader.hpp"

namespace cad::app {
namespace {

[[nodiscard]] QString cell(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item == nullptr ? QString{} : item->text().trimmed();
}

void setCell(QTableWidget* table, int row, int column, const QString& text) {
    table->setItem(row, column, new QTableWidgetItem{text});
}

/// The title block's sheet number and title, by the tags offices use.
struct TitleFields {
    QString number;
    QString title;
};

[[nodiscard]] TitleFields titleFieldsOf(const DrawingReading& reading) {
    TitleFields fields;
    for (const TitleBlockReading& block : reading.titleBlocks) {
        for (const auto& [tag, value] : block.fields) {
            const QString key = QString::fromStdString(tag).toUpper();
            const QString text = QString::fromStdString(value);
            if (fields.number.isEmpty() &&
                (key.startsWith(QStringLiteral("SHEET")) || key == QStringLiteral("DWGNO") ||
                 key == QStringLiteral("NUMBER") || key == QStringLiteral("SHT")) &&
                !key.contains(QStringLiteral("TITLE")) && text.size() <= 12) {
                fields.number = text;
            } else if (key.contains(QStringLiteral("TITLE")) || key == QStringLiteral("DESC") ||
                       key.startsWith(QStringLiteral("DRAWING"))) {
                fields.title += (fields.title.isEmpty() ? "" : " ") + text;
            }
        }
        if (!fields.number.isEmpty()) {
            break;
        }
    }
    return fields;
}

}  // namespace

ProjectDialog::ProjectDialog(ProjectIndex& index, const QString& folder, QWidget* parent)
    : QDialog(parent), index_(index), folder_(folder) {
    setWindowTitle(tr("Project"));
    resize(760, 560);

    auto* layout = new QVBoxLayout{this};
    auto* form = new QFormLayout;
    name_ = new QLineEdit{this};
    discipline_ = new QLineEdit{this};
    discipline_->setPlaceholderText(tr("M, E, A..."));
    discipline_->setMaximumWidth(80);
    template_ = new QLineEdit{this};
    template_->setPlaceholderText(tr("Drawing a new sheet is made from, optional"));
    form->addRow(tr("Project"), name_);
    form->addRow(tr("Discipline"), discipline_);
    form->addRow(tr("Sheet template"), template_);
    layout->addLayout(form);

    auto* sheetsLabel = new QLabel{tr("Sheets"), this};
    layout->addWidget(sheetsLabel);
    sheets_ = new QTableWidget{0, 6, this};
    sheets_->setHorizontalHeaderLabels(
        {tr("Number"), tr("Title"), tr("File"), tr("Layout"), tr("Background"), tr("Status")});
    sheets_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    sheets_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    sheets_->verticalHeader()->setVisible(false);
    layout->addWidget(sheets_, 3);
    auto* sheetButtons = new QHBoxLayout;
    auto* addSheet = new QPushButton{tr("Add sheet"), this};
    auto* removeSheet = new QPushButton{tr("Remove sheet"), this};
    auto* scan = new QPushButton{tr("List drawings in folder"), this};
    scan->setToolTip(tr("Add every drawing in the folder that is not listed yet, reading its "
                        "number and title from its title block."));
    sheetButtons->addWidget(addSheet);
    sheetButtons->addWidget(removeSheet);
    sheetButtons->addStretch();
    sheetButtons->addWidget(scan);
    layout->addLayout(sheetButtons);

    layout->addWidget(new QLabel{tr("Backgrounds"), this});
    backgrounds_ = new QTableWidget{0, 2, this};
    backgrounds_->setHorizontalHeaderLabels({tr("Name"), tr("File")});
    backgrounds_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    backgrounds_->verticalHeader()->setVisible(false);
    layout->addWidget(backgrounds_, 1);
    auto* backgroundButtons = new QHBoxLayout;
    auto* addBackground = new QPushButton{tr("Add background"), this};
    auto* removeBackground = new QPushButton{tr("Remove background"), this};
    backgroundButtons->addWidget(addBackground);
    backgroundButtons->addWidget(removeBackground);
    backgroundButtons->addStretch();
    layout->addLayout(backgroundButtons);

    layout->addWidget(new QLabel{tr("Notes for the assistant"), this});
    notes_ = new QPlainTextEdit{this};
    notes_->setPlaceholderText(
        tr("What the drawings cannot say: conventions, client rules, what is out of scope."));
    notes_->setMaximumHeight(80);
    layout->addWidget(notes_);

    status_ = new QLabel{this};
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* buttons = new QDialogButtonBox{QDialogButtonBox::Save | QDialogButtonBox::Cancel, this};
    layout->addWidget(buttons);

    connect(addSheet, &QPushButton::clicked, this, &ProjectDialog::onAddSheet);
    connect(removeSheet, &QPushButton::clicked, this, &ProjectDialog::onRemoveSheet);
    connect(scan, &QPushButton::clicked, this, &ProjectDialog::onScan);
    connect(addBackground, &QPushButton::clicked, this, &ProjectDialog::onAddBackground);
    connect(removeBackground, &QPushButton::clicked, this, &ProjectDialog::onRemoveBackground);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProjectDialog::onSave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (index_.isOpen()) {
        path_ = index_.path();
        folder_ = index_.folder();
        fill(index_.project());
        status_->setText(tr("Editing %1").arg(QDir::toNativeSeparators(path_)));
    } else {
        path_ = QDir{folder_}.filePath(QString::fromUtf8(kProjectFileName));
        Project fresh;
        fresh.name = QFileInfo{folder_}.fileName().toStdString();
        fill(fresh);
        status_->setText(tr("A new project will be written to %1")
                             .arg(QDir::toNativeSeparators(path_)));
    }
}

void ProjectDialog::fill(const Project& project) {
    name_->setText(QString::fromStdString(project.name));
    discipline_->setText(QString::fromStdString(project.discipline));
    template_->setText(QString::fromStdString(project.sheetTemplate));
    notes_->setPlainText(QString::fromStdString(project.notes));
    sheets_->setRowCount(0);
    for (const ProjectSheet& sheet : project.sheets) {
        addSheetRow(sheet);
    }
    backgrounds_->setRowCount(0);
    for (const ProjectBackground& background : project.backgrounds) {
        addBackgroundRow(background);
    }
}

void ProjectDialog::addSheetRow(const ProjectSheet& sheet) {
    const int row = sheets_->rowCount();
    sheets_->insertRow(row);
    setCell(sheets_, row, 0, QString::fromStdString(sheet.number));
    setCell(sheets_, row, 1, QString::fromStdString(sheet.title));
    setCell(sheets_, row, 2, QString::fromStdString(sheet.file));
    setCell(sheets_, row, 3, QString::fromStdString(sheet.layout));
    setCell(sheets_, row, 4, QString::fromStdString(sheet.background));
    setCell(sheets_, row, 5, QString::fromStdString(sheet.status));
}

void ProjectDialog::addBackgroundRow(const ProjectBackground& background) {
    const int row = backgrounds_->rowCount();
    backgrounds_->insertRow(row);
    setCell(backgrounds_, row, 0, QString::fromStdString(background.name));
    setCell(backgrounds_, row, 1, QString::fromStdString(background.file));
}

Project ProjectDialog::project() const {
    Project project;
    project.name = name_->text().trimmed().toStdString();
    project.discipline = discipline_->text().trimmed().toStdString();
    project.sheetTemplate = template_->text().trimmed().toStdString();
    project.notes = notes_->toPlainText().trimmed().toStdString();
    for (int row = 0; row < sheets_->rowCount(); ++row) {
        ProjectSheet sheet;
        sheet.number = cell(sheets_, row, 0).toStdString();
        sheet.title = cell(sheets_, row, 1).toStdString();
        sheet.file = cell(sheets_, row, 2).toStdString();
        sheet.layout = cell(sheets_, row, 3).toStdString();
        sheet.background = cell(sheets_, row, 4).toStdString();
        sheet.status = cell(sheets_, row, 5).toStdString();
        if (!sheet.number.empty()) {
            project.sheets.push_back(std::move(sheet));
        }
    }
    for (int row = 0; row < backgrounds_->rowCount(); ++row) {
        ProjectBackground background;
        background.name = cell(backgrounds_, row, 0).toStdString();
        background.file = cell(backgrounds_, row, 1).toStdString();
        if (!background.name.empty()) {
            project.backgrounds.push_back(std::move(background));
        }
    }
    project.sortSheets();
    return project;
}

void ProjectDialog::onAddSheet() {
    ProjectSheet sheet;
    // The next number in the pattern of the last one: M2.1 after M1.1 is a
    // guess, but a guess in the right shape is quicker to fix than a blank.
    if (sheets_->rowCount() > 0) {
        const QString last = cell(sheets_, sheets_->rowCount() - 1, 0);
        const int dot = last.lastIndexOf(QLatin1Char('.'));
        bool ok = false;
        const int minor = dot < 0 ? 0 : last.mid(dot + 1).toInt(&ok);
        sheet.number = (ok ? last.left(dot + 1) + QString::number(minor + 1) : last).toStdString();
    } else {
        sheet.number = (discipline_->text().trimmed() + QStringLiteral("0.1")).toStdString();
    }
    addSheetRow(sheet);
    sheets_->setCurrentCell(sheets_->rowCount() - 1, 1);
    sheets_->editItem(sheets_->item(sheets_->rowCount() - 1, 1));
}

void ProjectDialog::onRemoveSheet() {
    const int row = sheets_->currentRow();
    if (row >= 0) {
        sheets_->removeRow(row);
    }
}

void ProjectDialog::onAddBackground() {
    addBackgroundRow(ProjectBackground{});
    backgrounds_->setCurrentCell(backgrounds_->rowCount() - 1, 0);
    backgrounds_->editItem(backgrounds_->item(backgrounds_->rowCount() - 1, 0));
}

void ProjectDialog::onRemoveBackground() {
    const int row = backgrounds_->currentRow();
    if (row >= 0) {
        backgrounds_->removeRow(row);
    }
}

void ProjectDialog::onScan() {
    // Every drawing in the folder not yet listed, with what its title block
    // says it is. Files are the unit here: an office that keeps several
    // sheets in one file lists them by hand with the layout filled in.
    QStringList listed;
    for (int row = 0; row < sheets_->rowCount(); ++row) {
        listed << cell(sheets_, row, 2).toLower();
    }
    const QDir folder{folder_};
    const QStringList drawings = folder.entryList(
        {QStringLiteral("*.dwg"), QStringLiteral("*.dxf")}, QDir::Files, QDir::Name);
    int added = 0;
    QStringList problems;
    for (const QString& file : drawings) {
        if (listed.contains(file.toLower())) {
            continue;
        }
        status_->setText(tr("Reading %1...").arg(file));
        status_->repaint();
        ProjectSheet sheet;
        sheet.file = file.toStdString();
        try {
            io::XrefLoadOptions options;
            options.translator = nullptr;
            options.maxDepth = 0;
            io::DrawingLoadResult loaded = io::loadDrawingWithReferences(
                std::filesystem::path{folder.absoluteFilePath(file).toStdWString()}, options);
            if (loaded.database != nullptr) {
                DrawingReadingOptions quick;
                quick.budget = std::chrono::milliseconds{0};
                const TitleFields fields = titleFieldsOf(readDrawing(*loaded.database, quick));
                sheet.number = fields.number.toStdString();
                sheet.title = fields.title.toStdString();
            }
        } catch (const std::exception& error) {
            problems << QStringLiteral("%1: %2").arg(file, QString::fromUtf8(error.what()));
        }
        if (sheet.number.empty()) {
            sheet.number = QFileInfo{file}.completeBaseName().toStdString();
        }
        addSheetRow(sheet);
        ++added;
    }
    QString report = tr("%n drawing(s) added.", "", added);
    if (!problems.isEmpty()) {
        report += QLatin1Char(' ') + problems.join(QStringLiteral("; "));
    }
    status_->setText(report);
}

void ProjectDialog::onSave() {
    Project project = this->project();
    if (project.name.empty()) {
        QMessageBox::warning(this, tr("Project"), tr("Give the project a name."));
        return;
    }
    if (!index_.isOpen() || index_.path() != path_) {
        std::string error;
        if (!saveProject(project, std::filesystem::path{path_.toStdWString()}, &error)) {
            QMessageBox::warning(this, tr("Project"), QString::fromStdString(error));
            return;
        }
        QString problem;
        if (!index_.open(path_, &problem)) {
            QMessageBox::warning(this, tr("Project"), problem);
            return;
        }
    } else {
        index_.project() = std::move(project);
        QString problem;
        if (!index_.save(&problem)) {
            QMessageBox::warning(this, tr("Project"), problem);
            return;
        }
    }
    accept();
}

}  // namespace cad::app
