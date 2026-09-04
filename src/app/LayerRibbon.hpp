#pragma once

#include <QWidget>

#include "cad/db/Handle.hpp"

class QComboBox;
class QToolButton;

namespace cad {
class Layer;
}

namespace cad::app {

class Document;

/// The Layers control that lives in the ribbon.
///
/// The layer a drafter is *drawing on* changes many times an hour, and the
/// three switches they reach for — off, frozen, locked — are aimed at the
/// current layer almost every time. That belongs in the reach of the drawing
/// hand, not in a palette down the side competing with the drawing for width.
///
/// The full manager is still one button away, because sixty layers need
/// filtering, sorting and per-layer colour, and none of that fits in a strip.
class LayerRibbon final : public QWidget {
    Q_OBJECT

public:
    explicit LayerRibbon(QWidget* parent = nullptr);

    void setDocument(Document* document);

    /// Repopulate from the database. Cheap enough to call on any change.
    void refresh();

Q_SIGNALS:
    /// A layer's visibility or the current layer changed.
    void layersChanged();

    /// The full layer manager was asked for.
    void manageRequested();

private:
    /// The layer the switches act on: the current one.
    [[nodiscard]] Handle currentLayerHandle() const;

    void onLayerChosen(int index);
    void toggleCurrent(const QString& operation, void (*mutate)(Layer&, bool), bool on);

    Document* document_ = nullptr;
    QComboBox* combo_ = nullptr;
    QToolButton* on_ = nullptr;
    QToolButton* freeze_ = nullptr;
    QToolButton* lock_ = nullptr;
    /// Set while refresh() writes into the widgets, so their change signals do
    /// not write the display state straight back into the drawing.
    bool populating_ = false;
};

}  // namespace cad::app
