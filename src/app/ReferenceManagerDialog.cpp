#include "ReferenceManagerDialog.hpp"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Document.hpp"
#include "Theme.hpp"
#include "cad/db/Transaction.hpp"

namespace cad::app {
namespace {

constexpr int kHandleRole = Qt::UserRole + 1;

}  // namespace

ReferenceManagerDialog::ReferenceManagerDialog(Document& document, QWidget* parent)
    : QDialog{parent}, document_{document} {
    setWindowTitle(tr("References"));
    setMinimumSize(720, 360);

    auto* outer = new QVBoxLayout{this};

    summary_ = new QLabel{this};
    summary_->setStyleSheet(
        QString{"color: %1;"}.arg(currentTheme().textMuted.name()));

    tree_ = new QTreeWidget{this};
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("Type"), tr("Status"), tr("Used"), tr("Path")});
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree_->header()->setStretchLastSection(true);

    locate_ = new QPushButton{tr("Locate…"), this};
    locate_->setEnabled(false);
    locate_->setToolTip(tr("Point this reference at a file on disk"));

    auto* buttons = new QDialogButtonBox{QDialogButtonBox::Close, this};
    auto* row = new QHBoxLayout;
    row->addWidget(locate_);
    row->addStretch(1);
    row->addWidget(buttons);

    outer->addWidget(summary_);
    outer->addWidget(tree_, 1);
    outer->addLayout(row);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this,
            &ReferenceManagerDialog::onSelectionChanged);
    // Double-clicking a row is the same as pressing Locate, because a row a
    // user has just been told is broken is a row they are about to fix.
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem*, int) { onLocate(); });
    connect(locate_, &QPushButton::clicked, this, &ReferenceManagerDialog::onLocate);

    reload();
}

void ReferenceManagerDialog::reload() {
    nodes_ = io::attachedFiles(document_.db());
    tree_->clear();

    std::size_t missing = 0;
    std::size_t undrawn = 0;
    for (const io::AttachedFileNode& node : nodes_) {
        auto* item = new QTreeWidgetItem{tree_};
        item->setText(0, QString::fromStdString(node.kind));
        item->setText(1, node.found ? tr("Found") : tr("Missing"));
        // A path that nothing places is a leftover — worth showing, because it
        // is also the one a user can ignore. A path placed only inside a block
        // nothing inserts is worse than a leftover: it looks used, and repairing
        // it changes nothing on the screen, so it says so rather than counting
        // a placement that is never drawn.
        QString placement;
        if (node.placements == 0) {
            placement = tr("unused");
        } else if (node.drawnPlacements == 0) {
            placement = tr("%1×, none drawn").arg(node.placements);
        } else if (node.drawnPlacements < node.placements) {
            placement = tr("%1× (%2 drawn)").arg(node.placements).arg(node.drawnPlacements);
        } else {
            placement = tr("%1×").arg(node.placements);
        }
        item->setText(2, placement);
        if (!node.unplacedBlocks.empty()) {
            QStringList blocks;
            for (const std::string& block : node.unplacedBlocks) {
                blocks << QString::fromStdString(block);
            }
            item->setToolTip(2, tr("Placed in block %1, which no layout inserts, so it is "
                                   "not drawn anywhere.")
                                    .arg(blocks.join(tr(", "))));
            if (node.drawnPlacements == 0) {
                item->setForeground(2, currentTheme().missingReference);
            }
        }
        item->setText(3, QString::fromStdString(node.storedPath));
        item->setToolTip(3, node.found
                                ? tr("Resolved to %1").arg(
                                      QString::fromStdString(node.resolvedPath.string()))
                                : QString::fromStdString(node.problem));
        item->setData(0, kHandleRole,
                      static_cast<qulonglong>(node.definitionHandle.value()));
        if (!node.found) {
            item->setForeground(1, currentTheme().missingReference);
            ++missing;
        }
        if (node.placements > 0 && node.drawnPlacements == 0) {
            ++undrawn;
        }
    }

    QString summary;
    if (nodes_.empty()) {
        summary = tr("This drawing has no attached images or underlays.");
    } else {
        summary = tr("%1 attached file(s), %2 missing. Select one and choose Locate to "
                     "point it at a file that exists.")
                      .arg(nodes_.size())
                      .arg(missing);
        // Said outright, because the alternative is a user repairing a path
        // correctly and concluding the repair failed when nothing appears.
        if (undrawn > 0) {
            summary += tr(" %1 of them is placed only inside a block that no layout "
                          "inserts, so repairing its path will not make it appear.")
                           .arg(undrawn);
        }
    }
    summary_->setText(summary);
    onSelectionChanged();
}

Handle ReferenceManagerDialog::selectedDefinition() const {
    const QList<QTreeWidgetItem*> selected = tree_->selectedItems();
    if (selected.isEmpty()) {
        return Handle{};
    }
    return Handle{selected.front()->data(0, kHandleRole).toULongLong()};
}

void ReferenceManagerDialog::onSelectionChanged() {
    locate_->setEnabled(!selectedDefinition().isNull());
}

void ReferenceManagerDialog::onLocate() {
    const Handle definition = selectedDefinition();
    if (definition.isNull()) {
        return;
    }

    // Opened beside the drawing, which is where the file usually is when the
    // stored path is stale: a project folder copied from another machine has
    // the right file names and the wrong directories.
    QString startIn;
    if (!document_.filePath.empty()) {
        startIn = QFileInfo{QString::fromStdString(document_.filePath)}.absolutePath();
    }

    const QString chosen = QFileDialog::getOpenFileName(
        this, tr("Locate reference"), startIn,
        tr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.gif);;"
           "Documents (*.pdf *.dwf *.dgn);;All files (*)"));
    if (chosen.isEmpty()) {
        return;
    }

    // Through a transaction, because it is a change to the drawing like any
    // other: it belongs on the undo stack and it makes the file dirty. A user
    // who repoints an image at the wrong file must be able to press Ctrl+Z.
    document_.transactions().run("Locate reference", [&](Transaction& transaction) {
        if (DbObject* object = document_.db().getObject(definition)) {
            // Snapshotted before the path changes, which is what makes the
            // undo work rather than merely appear on the stack.
            static_cast<void>(transaction.openForWrite(*object));
        }
        static_cast<void>(io::repathAttachedFile(
            document_.db(), definition, std::filesystem::path{chosen.toStdWString()}));
    });

    // Re-resolve everything, not only this one: a folder that was just named
    // may be where several of the others live too.
    io::XrefLoadOptions options;
    if (!document_.filePath.empty()) {
        options.searchPaths.push_back(
            std::filesystem::path{document_.filePath}.parent_path());
    }
    options.searchPaths.push_back(
        std::filesystem::path{chosen.toStdWString()}.parent_path());
    static_cast<void>(io::resolveAttachedFiles(document_.db(), document_.filePath, options));

    reload();
    Q_EMIT referencesChanged();
}

}  // namespace cad::app
