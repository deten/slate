#include "ProjectPanel.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ProjectIndex.hpp"

namespace cad::app {
namespace {

constexpr int kPathRole = Qt::UserRole;

}  // namespace

ProjectPanel::ProjectPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(4, 4, 4, 4);

    title_ = new QLabel{tr("No project"), this};
    title_->setWordWrap(true);
    layout->addWidget(title_);

    tree_ = new QTreeWidget{this};
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Sheet"), tr("Title"), tr("State")});
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(tree_, 1);

    auto* buttons = new QHBoxLayout;
    open_ = new QPushButton{tr("Open sheet"), this};
    auto* background = new QPushButton{tr("Attach background"), this};
    background->setToolTip(tr("Overlay the open sheet's background as the office does: placed "
                              "as on the sibling sheets, with the project's layers frozen."));
    auto* edit = new QPushButton{tr("Project…"), this};
    edit->setToolTip(tr("Make or change the project: its sheets, backgrounds and notes."));
    buttons->addWidget(open_);
    buttons->addWidget(background);
    buttons->addStretch();
    buttons->addWidget(edit);
    layout->addLayout(buttons);
    connect(background, &QPushButton::clicked, this, &ProjectPanel::attachBackgroundRequested);

    note_ = new QLabel{this};
    note_->setWordWrap(true);
    layout->addWidget(note_);

    connect(tree_, &QTreeWidget::itemActivated, this, &ProjectPanel::onItemActivated);
    connect(open_, &QPushButton::clicked, this, &ProjectPanel::onOpenClicked);
    connect(edit, &QPushButton::clicked, this, &ProjectPanel::editRequested);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            [this] { open_->setEnabled(!tree_->selectedItems().isEmpty()); });
    open_->setEnabled(false);
}

void ProjectPanel::setIndex(ProjectIndex* index) {
    index_ = index;
    refresh();
}

void ProjectPanel::setCurrentFile(const QString& drawingPath) {
    currentFile_ = drawingPath.isEmpty() ? QString{} : QFileInfo{drawingPath}.absoluteFilePath();
    refresh();
}

void ProjectPanel::refresh() {
    tree_->clear();
    if (index_ == nullptr || !index_->isOpen()) {
        title_->setText(tr("No project. Tools > Project… makes one beside the drawings."));
        note_->clear();
        return;
    }
    const Project& project = index_->project();
    title_->setText(QStringLiteral("<b>%1</b>")
                        .arg(QString::fromStdString(project.name).toHtmlEscaped()));

    int missing = 0;
    int stale = 0;
    for (const ProjectSheet& sheet : project.sheets) {
        auto* item = new QTreeWidgetItem{tree_};
        item->setText(0, QString::fromStdString(sheet.number));
        item->setText(1, QString::fromStdString(sheet.title));
        const QString absolute = index_->absolutePath(QString::fromStdString(sheet.file));
        item->setData(0, kPathRole, absolute);
        item->setToolTip(1, absolute);

        // What a person wants to know of a sheet without opening it: is the
        // file there, and has the drawing behind it moved on.
        QStringList state;
        if (!sheet.status.empty()) {
            state << QString::fromStdString(sheet.status);
        }
        const QFileInfo file{absolute};
        if (absolute.isEmpty() || !file.exists()) {
            state << tr("file missing");
            ++missing;
        } else if (!sheet.background.empty()) {
            if (const ProjectBackground* background = project.findBackground(sheet.background)) {
                const QFileInfo behind{index_->absolutePath(QString::fromStdString(background->file))};
                if (!behind.exists()) {
                    state << tr("background %1 missing").arg(QString::fromStdString(sheet.background));
                } else if (behind.lastModified() > file.lastModified()) {
                    state << tr("background %1 newer").arg(QString::fromStdString(sheet.background));
                    ++stale;
                }
            }
        }
        item->setText(2, state.join(QStringLiteral(", ")));
        if (!absolute.isEmpty() && absolute == currentFile_) {
            QFont bold = item->font(0);
            bold.setBold(true);
            for (int column = 0; column < 3; ++column) {
                item->setFont(column, bold);
            }
            item->setText(2, state.isEmpty() ? tr("open") : tr("open, %1").arg(item->text(2)));
        }
    }
    tree_->resizeColumnToContents(0);

    QStringList notes;
    notes << tr("%n sheet(s)", "", static_cast<int>(project.sheets.size()));
    if (missing > 0) {
        notes << tr("%n file(s) missing", "", missing);
    }
    if (stale > 0) {
        notes << tr("%n background(s) changed since the sheet was saved", "", stale);
    }
    note_->setText(notes.join(QStringLiteral(", ")));
}

void ProjectPanel::onItemActivated(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }
    const QString path = item->data(0, kPathRole).toString();
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        Q_EMIT openSheetRequested(path);
    }
}

void ProjectPanel::onOpenClicked() {
    const QList<QTreeWidgetItem*> selected = tree_->selectedItems();
    if (!selected.isEmpty()) {
        onItemActivated(selected.front(), 0);
    }
}

}  // namespace cad::app
