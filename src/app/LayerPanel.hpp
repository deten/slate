#pragma once

#include <QWidget>

#include "cad/db/Handle.hpp"

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace cad::app {

class Document;

/// The layer manager.
///
/// Read-only layer lists are the reason a drawing with sixty layers is
/// unusable: the first thing anyone does with someone else's drawing is turn
/// most of it off. So on, frozen, locked and plot are one click each, colour
/// is one double-click, and the current layer is one more.
///
/// Every change goes through a transaction, so turning a layer off undoes like
/// anything else — which matters more than it sounds, because turning off the
/// wrong layer on a large drawing is otherwise a hunt to find it again.
class LayerPanel final : public QWidget {
    Q_OBJECT

public:
    explicit LayerPanel(QWidget* parent = nullptr);

    void setDocument(Document* document);
    void refresh();

Q_SIGNALS:
    /// Visibility or appearance changed, so the canvas needs redrawing.
    void layersChanged();

private:
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void makeCurrentLayer(QTreeWidgetItem* item);

    /// Apply a change to one layer inside its own transaction.
    template <typename F>
    void editLayer(Handle handle, const QString& operation, F&& mutate);

    Document* document_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLineEdit* filter_ = nullptr;
    bool populating_ = false;
};

}  // namespace cad::app
