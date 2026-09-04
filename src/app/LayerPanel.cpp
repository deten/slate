#include "LayerPanel.hpp"

#include <QColorDialog>
#include <QHeaderView>
#include <QLineEdit>
#include <QPixmap>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Document.hpp"
#include "LayerOrdering.hpp"
#include "Theme.hpp"
#include "cad/db/Layer.hpp"

namespace cad::app {
namespace {

/// Columns, in the order other CAD software's layer manager uses them. Muscle memory is a
/// real feature: a drafter reaches for the third toggle without reading it.
enum Column : int { NameColumn = 0, OnColumn, FrozenColumn, LockedColumn, PlotColumn, ColorColumn };

constexpr int kHandleRole = Qt::UserRole + 1;

}  // namespace

LayerPanel::LayerPanel(QWidget* parent) : QWidget{parent} {
    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    filter_ = new QLineEdit{this};
    filter_->setPlaceholderText(tr("Filter layers…"));
    filter_->setClearButtonEnabled(true);
    filter_->setContentsMargins(6, 4, 6, 4);

    tree_ = new QTreeWidget{this};
    tree_->setColumnCount(6);
    tree_->setHeaderLabels({tr("Name"), tr("On"), tr("Frz"), tr("Lck"), tr("Plot"), tr("Colour")});
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    for (int column = OnColumn; column <= ColorColumn; ++column) {
        tree_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    // Stretch means "whatever is left over", and with five fixed columns beside
    // it there was nothing left: every layer name rendered as an ellipsis, in
    // the one column anybody reads. A floor: the panel is a layer *manager*, so
    // it is worth the width it needs to show a name like
    // "A-EQUIP-NEW-CLEARANCE" rather than three dots.
    constexpr int kNameFloor = 210;
    tree_->header()->setMinimumSectionSize(0);
    setMinimumWidth(kNameFloor + tree_->header()->sectionSize(OnColumn) * 5);

    tree_->setToolTip(tr("Click a box to toggle. Double-click a name to make it current, "
                         "or a colour to change it."));

    connect(tree_, &QTreeWidget::itemChanged, this, &LayerPanel::onItemChanged);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &LayerPanel::onItemDoubleClicked);
    connect(filter_, &QLineEdit::textChanged, this, [this] { refresh(); });

    layout->addWidget(filter_);
    layout->addWidget(tree_, 1);
}

void LayerPanel::setDocument(Document* document) {
    document_ = document;
    filter_->clear();
    refresh();
}

void LayerPanel::refresh() {
    // As in the properties palette: populating the tree emits itemChanged for
    // every cell, and without this the panel writes its own display state back
    // into the drawing.
    populating_ = true;
    tree_->clear();

    if (document_ == nullptr) {
        populating_ = false;
        return;
    }

    const QString needle = filter_->text().trimmed();
    const Database& db = document_->db();
    const std::string& currentName = db.settings.currentLayerName;

    for (const Handle handle : layersByName(db)) {
        const auto* layer = db.get<Layer>(handle);
        if (layer == nullptr || layer->erased) {
            continue;
        }
        const QString name = QString::fromStdString(layer->name);
        if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }

        auto* item = new QTreeWidgetItem{tree_};
        item->setData(NameColumn, kHandleRole, static_cast<qulonglong>(handle.value()));
        item->setText(NameColumn, name);

        // The current layer is what new geometry lands on, so it has to be
        // obvious at a glance rather than discoverable.
        if (layer->name == currentName) {
            QFont font = item->font(NameColumn);
            font.setBold(true);
            item->setFont(NameColumn, font);
            item->setText(NameColumn, name + tr("  (current)"));
        }

        item->setCheckState(OnColumn, layer->isOff ? Qt::Unchecked : Qt::Checked);
        item->setCheckState(FrozenColumn, layer->isFrozen ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(LockedColumn, layer->isLocked ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(PlotColumn, layer->isPlottable ? Qt::Checked : Qt::Unchecked);

        QPixmap swatch{12, 12};
        swatch.fill(QColor::fromRgb(static_cast<QRgb>(layer->color.toRgb())));
        item->setIcon(ColorColumn, QIcon{swatch});
        item->setText(ColorColumn, layer->color.method() == ColorMethod::Index
                                       ? QString::number(layer->color.index())
                                       : QStringLiteral("#%1")
                                             .arg(layer->color.rgb(), 6, 16, QLatin1Char('0'))
                                             .toUpper());

        // A layer that is off or frozen is dimmed, so the state reads without
        // checking three boxes.
        if (!layer->isVisible()) {
            item->setForeground(NameColumn, currentTheme().textMuted);
        }
    }

    populating_ = false;
}

template <typename F>
void LayerPanel::editLayer(Handle handle, const QString& operation, F&& mutate) {
    if (document_ == nullptr) {
        return;
    }
    auto* layer = document_->db().get<Layer>(handle);
    if (layer == nullptr) {
        return;
    }

    Transaction& transaction = document_->transactions().begin(operation.toStdString());
    try {
        transaction.openForWrite(*layer);
        mutate(*layer);
        document_->db().notifyModified(handle);
        document_->transactions().commit(transaction);
    } catch (const std::exception&) {
        document_->transactions().abort(transaction);
    }

    refresh();
    Q_EMIT layersChanged();
}

void LayerPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (populating_ || document_ == nullptr || item == nullptr) {
        return;
    }
    const Handle handle{item->data(NameColumn, kHandleRole).toULongLong()};
    const bool checked = item->checkState(column) == Qt::Checked;

    switch (column) {
        case OnColumn:
            editLayer(handle, tr("Layer on/off"),
                      [checked](Layer& layer) { layer.isOff = !checked; });
            break;
        case FrozenColumn:
            editLayer(handle, tr("Freeze layer"),
                      [checked](Layer& layer) { layer.isFrozen = checked; });
            break;
        case LockedColumn:
            editLayer(handle, tr("Lock layer"),
                      [checked](Layer& layer) { layer.isLocked = checked; });
            break;
        case PlotColumn:
            editLayer(handle, tr("Layer plot"),
                      [checked](Layer& layer) { layer.isPlottable = checked; });
            break;
        default:
            break;
    }
}

void LayerPanel::onItemDoubleClicked(QTreeWidgetItem* item, int column) {
    if (document_ == nullptr || item == nullptr) {
        return;
    }
    const Handle handle{item->data(NameColumn, kHandleRole).toULongLong()};

    if (column == ColorColumn) {
        const auto* layer = document_->db().get<Layer>(handle);
        if (layer == nullptr) {
            return;
        }
        const QColor initial = QColor::fromRgb(static_cast<QRgb>(layer->color.toRgb()));
        const QColor chosen = QColorDialog::getColor(initial, this, tr("Layer colour"));
        if (!chosen.isValid()) {
            return;
        }
        editLayer(handle, tr("Layer colour"), [chosen](Layer& target) {
            target.color = Color::fromRgb(chosen.red(), chosen.green(), chosen.blue());
        });
        return;
    }

    if (column == NameColumn) {
        makeCurrentLayer(item);
    }
}

void LayerPanel::makeCurrentLayer(QTreeWidgetItem* item) {
    const Handle handle{item->data(NameColumn, kHandleRole).toULongLong()};
    const auto* layer = document_->db().get<Layer>(handle);
    if (layer == nullptr) {
        return;
    }
    // The current layer is a drawing setting rather than an object, so it is
    // not part of the undo stack — matching other CAD software, where CLAYER is a system
    // variable and undoing a line does not move the cursor back to its layer.
    document_->db().settings.currentLayerName = layer->name;
    refresh();
    Q_EMIT layersChanged();
}

}  // namespace cad::app
