#include "LayerRibbon.hpp"

#include "RibbonIcons.hpp"
#include "Theme.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QPixmap>
#include <QToolButton>
#include <QVBoxLayout>

#include "Document.hpp"
#include "LayerOrdering.hpp"
#include "RibbonLayout.hpp"
#include "cad/db/Layer.hpp"

namespace cad::app {
namespace {

constexpr int kHandleRole = Qt::UserRole + 1;

/// A checkable strip button that reads as a state rather than an action.
[[nodiscard]] QToolButton* stateButton(QWidget* parent, const QString& label,
                                       const QString& tooltip, const QString& icon) {
    auto* button = new QToolButton{parent};
    button->setText(label);
    // Pictured like every other small button in the ribbon. Without one these
    // three read as a column of bare words beside rows that all have icons,
    // which looks like something that has not been finished rather than like a
    // different kind of control.
    const Theme& theme = currentTheme();
    button->setIcon(ribbonIcon(icon, label, 16, theme.text, theme.accent));
    button->setIconSize(QSize{16, 16});
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // Styled as what it is: a small ribbon button. Without the name it fell
    // through to the application-wide tool button rule, whose vertical padding
    // is sized for a toolbar and clipped these three labels' descenders.
    button->setObjectName(QStringLiteral("ribbonSmall"));
    button->setToolTip(tooltip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(kRibbonSmallButtonHeight);
    // As everywhere in the ribbon: clicking a control here must not take the
    // keyboard away from the command line.
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

}  // namespace

LayerRibbon::LayerRibbon(QWidget* parent) : QWidget{parent} {
    auto* row = new QHBoxLayout{this};
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    combo_ = new QComboBox{this};
    // Wide enough for a layer name a drafter can recognise, and no wider. This
    // sits in the Home tab's first group, and Home is the tab that runs out of
    // window first -- 190px here was pushing the annotation tools off the right
    // edge. Names longer than this elide in the closed combo and are shown in
    // full in the drop-down, where there is room.
    combo_->setMinimumWidth(140);
    combo_->setMaximumWidth(190);
    combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo_->setMaxVisibleItems(24);
    combo_->setFocusPolicy(Qt::NoFocus);
    combo_->setToolTip(tr("The layer new geometry is drawn on"));
    connect(combo_, &QComboBox::activated, this, &LayerRibbon::onLayerChosen);

    auto* switches = new QWidget{this};
    auto* column = new QVBoxLayout{switches};
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(kRibbonColumnSpacing);
    column->setAlignment(Qt::AlignTop);

    on_ = stateButton(switches, tr("On"), tr("Show or hide the current layer"), QStringLiteral("LAYERON"));
    freeze_ = stateButton(switches, tr("Freeze"), tr("Freeze the current layer"), QStringLiteral("LAYERFREEZE"));
    lock_ = stateButton(switches, tr("Lock"), tr("Lock the current layer against editing"), QStringLiteral("LAYERLOCK"));
    column->addWidget(on_);
    column->addWidget(freeze_);
    column->addWidget(lock_);

    connect(on_, &QToolButton::toggled, this, [this](bool checked) {
        toggleCurrent(tr("Layer on/off"), [](Layer& layer, bool on) { layer.isOff = !on; },
                      checked);
    });
    connect(freeze_, &QToolButton::toggled, this, [this](bool checked) {
        toggleCurrent(tr("Freeze layer"),
                      [](Layer& layer, bool on) { layer.isFrozen = on; }, checked);
    });
    connect(lock_, &QToolButton::toggled, this, [this](bool checked) {
        toggleCurrent(tr("Lock layer"), [](Layer& layer, bool on) { layer.isLocked = on; },
                      checked);
    });

    auto* manage = new QToolButton{this};
    manage->setText(tr("Layer\nProperties"));
    {
        const Theme& theme = currentTheme();
        manage->setIcon(ribbonIcon(QStringLiteral("LAYER"), QStringLiteral("Layer"), 26,
                                   theme.text, theme.accent));
        manage->setIconSize(QSize{26, 26});
        manage->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    }
    manage->setToolTip(tr("Open the layer manager"));
    manage->setAutoRaise(true);
    manage->setCursor(Qt::PointingHandCursor);
    manage->setFocusPolicy(Qt::NoFocus);
    // Wide enough for "Properties" and as tall as the buttons beside it.
    manage->setFixedSize(90, kRibbonContentHeight);
    connect(manage, &QToolButton::clicked, this, &LayerRibbon::manageRequested);

    row->addWidget(manage);
    row->addWidget(combo_);
    row->addWidget(switches);

    refresh();
}

void LayerRibbon::setDocument(Document* document) {
    document_ = document;
    refresh();
}

Handle LayerRibbon::currentLayerHandle() const {
    if (document_ == nullptr || combo_->currentIndex() < 0) {
        return Handle{};
    }
    return Handle{combo_->currentData(kHandleRole).toULongLong()};
}

void LayerRibbon::refresh() {
    populating_ = true;
    combo_->clear();

    const bool hasDocument = document_ != nullptr;
    combo_->setEnabled(hasDocument);
    on_->setEnabled(hasDocument);
    freeze_->setEnabled(hasDocument);
    lock_->setEnabled(hasDocument);
    if (!hasDocument) {
        populating_ = false;
        return;
    }

    const Database& db = document_->db();
    const std::string& currentName = db.settings.currentLayerName;
    const Layer* current = nullptr;

    for (const Handle handle : layersByName(db)) {
        const auto* layer = db.get<Layer>(handle);
        if (layer == nullptr || layer->erased) {
            continue;
        }
        QPixmap swatch{12, 12};
        swatch.fill(QColor::fromRgb(static_cast<QRgb>(layer->color.toRgb())));

        combo_->addItem(QIcon{swatch}, QString::fromStdString(layer->name));
        combo_->setItemData(combo_->count() - 1, static_cast<qulonglong>(handle.value()),
                            kHandleRole);
        if (layer->name == currentName) {
            combo_->setCurrentIndex(combo_->count() - 1);
            current = layer;
        }
    }

    // The switches show the *current* layer's state, so they have to be
    // meaningless rather than stale when there is no current layer to show.
    const bool known = current != nullptr;
    on_->setChecked(known && !current->isOff);
    freeze_->setChecked(known && current->isFrozen);
    lock_->setChecked(known && current->isLocked);

    populating_ = false;
}

void LayerRibbon::onLayerChosen(int index) {
    if (populating_ || document_ == nullptr || index < 0) {
        return;
    }
    const auto* layer = document_->db().get<Layer>(
        Handle{combo_->itemData(index, kHandleRole).toULongLong()});
    if (layer == nullptr) {
        return;
    }
    // As in the layer manager: the current layer is a drawing setting rather
    // than an object, so it is not on the undo stack.
    document_->db().settings.currentLayerName = layer->name;
    refresh();
    Q_EMIT layersChanged();
}

void LayerRibbon::toggleCurrent(const QString& operation, void (*mutate)(Layer&, bool),
                                bool on) {
    if (populating_ || document_ == nullptr) {
        return;
    }
    const Handle handle = currentLayerHandle();
    auto* layer = document_->db().get<Layer>(handle);
    if (layer == nullptr) {
        return;
    }

    Transaction& transaction = document_->transactions().begin(operation.toStdString());
    try {
        transaction.openForWrite(*layer);
        mutate(*layer, on);
        document_->db().notifyModified(handle);
        document_->transactions().commit(transaction);
    } catch (const std::exception&) {
        document_->transactions().abort(transaction);
    }

    Q_EMIT layersChanged();
}

}  // namespace cad::app
