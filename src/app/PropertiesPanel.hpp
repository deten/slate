#pragma once

#include <QWidget>
#include <string>
#include <vector>

#include "cad/db/Property.hpp"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace cad::app {

class Document;

/// The properties palette.
///
/// Shows what the selection has in common and edits all of it at once. It knows
/// nothing about entity types: rows come from the core's property registry, so
/// an entity type contributed by a plugin appears here with no change to this
/// file. That indirection is the point — a palette with a switch over entity
/// types is a palette that has to be edited every time the model grows.
///
/// Editing goes through a transaction, so a change here undoes exactly like a
/// change made by a command.
class PropertiesPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget* parent = nullptr);

    void setDocument(Document* document);

    /// Rebuild from the document's current selection.
    void refresh();

Q_SIGNALS:
    /// A property was changed, so the canvas and the layer panel need redrawing.
    void propertyChanged();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    /// Fit the name column to its content, capped so the value column stays
    /// legible however narrow the panel is.
    void updateColumnWidths();

    void buildRow(QTreeWidgetItem* parent, const AggregatedProperty& row);
    void onItemChanged(QTreeWidgetItem* item, int column);

    /// Turn a value into what the cell shows.
    [[nodiscard]] QString displayText(const AggregatedProperty& row) const;

    /// Read a cell back, returning nothing when the text will not parse.
    [[nodiscard]] std::optional<PropertyValue> parseCell(const PropertyDefinition& definition,
                                                         const QString& text) const;

    /// Name of the symbol table record a Reference property points at.
    [[nodiscard]] QString referenceName(const PropertyDefinition& definition,
                                        Handle handle) const;

    /// Every record a Reference property could point at, for the drop-down.
    [[nodiscard]] const std::vector<Handle>* referenceCandidates(
        const PropertyDefinition& definition) const;

    void applyEdit(const PropertyDefinition& definition, const PropertyValue& value);

    Document* document_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* summary_ = nullptr;

    /// Rows currently displayed, indexed by the tree item's stored row index.
    std::vector<AggregatedProperty> rows_;

    /// Set while the tree is being rebuilt, so programmatic edits do not come
    /// back through the change handler and write themselves to the database.
    bool populating_ = false;
};

}  // namespace cad::app
