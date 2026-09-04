#include "PropertiesPanel.hpp"

#include <algorithm>

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>

#include "Document.hpp"
#include "Theme.hpp"
#include "cad/db/Layer.hpp"
#include "cad/math/Angle.hpp"

namespace cad::app {
namespace {

/// Shown where the selection disagrees. Editing the cell still sets them all,
/// which is the whole reason a varying row is offered rather than hidden.
constexpr const char* kVaries = "*VARIES*";

/// Role holding a row's index into `rows_`, so a changed cell can be traced
/// back to its definition without matching on the label.
constexpr int kRowIndexRole = Qt::UserRole + 1;

[[nodiscard]] QString formatNumber(double value, int decimals = 4) {
    QString text = QString::number(value, 'f', decimals);
    // Trailing zeros make a palette hard to scan; a whole number should read
    // as one.
    if (text.contains('.')) {
        while (text.endsWith('0')) {
            text.chop(1);
        }
        if (text.endsWith('.')) {
            text.chop(1);
        }
    }
    return text;
}

[[nodiscard]] QString formatPoint(const Vec3& point) {
    return QStringLiteral("%1, %2, %3")
        .arg(formatNumber(point.x), formatNumber(point.y), formatNumber(point.z));
}

[[nodiscard]] std::optional<Vec3> parsePoint(const QString& text) {
    const QStringList parts = text.split(',', Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts.size() > 3) {
        return std::nullopt;
    }
    bool okX = false;
    bool okY = false;
    const double x = parts[0].trimmed().toDouble(&okX);
    const double y = parts[1].trimmed().toDouble(&okY);
    if (!okX || !okY) {
        return std::nullopt;
    }
    double z = 0.0;
    if (parts.size() == 3) {
        bool okZ = false;
        z = parts[2].trimmed().toDouble(&okZ);
        if (!okZ) {
            return std::nullopt;
        }
    }
    return Vec3{x, y, z};
}

/// The palette shows colours by name where DWG has one, because "ByLayer" and
/// "red" are how a drafter talks about them.
[[nodiscard]] QString colorText(const Color& color) {
    switch (color.method()) {
        case ColorMethod::ByLayer:
            return QObject::tr("ByLayer");
        case ColorMethod::ByBlock:
            return QObject::tr("ByBlock");
        case ColorMethod::TrueColor:
            return QStringLiteral("#%1").arg(color.rgb(), 6, 16, QLatin1Char('0')).toUpper();
        case ColorMethod::Index:
        default:
            return QStringLiteral("%1").arg(color.index());
    }
}

[[nodiscard]] std::optional<Color> parseColor(const QString& raw) {
    const QString text = raw.trimmed();
    if (text.compare("ByLayer", Qt::CaseInsensitive) == 0) {
        return Color::byLayer();
    }
    if (text.compare("ByBlock", Qt::CaseInsensitive) == 0) {
        return Color::byBlock();
    }
    if (text.startsWith('#')) {
        bool ok = false;
        const auto rgb = text.mid(1).toUInt(&ok, 16);
        return ok ? std::optional<Color>{Color::fromTrueColor(rgb)} : std::nullopt;
    }
    bool ok = false;
    const int index = text.toInt(&ok);
    if (ok && index >= 0 && index <= 256) {
        return Color::fromIndex(index);
    }
    return std::nullopt;
}

/// The value column never gets less than this, whatever the names are doing.
constexpr int kMinimumValueWidth = 84;

/// Below this the panel cannot show a name and a value at once, so it is not
/// allowed to be squeezed further; the dock is closable if the width is needed.
constexpr int kMinimumPanelWidth = 210;

/// A property name needs at least this much before truncating is the only
/// option left.
constexpr int kMinimumNameWidth = 96;

}  // namespace

PropertiesPanel::PropertiesPanel(QWidget* parent) : QWidget{parent} {
    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    summary_ = new QLabel{tr("No selection"), this};
    summary_->setContentsMargins(8, 6, 8, 6);
    summary_->setStyleSheet(
        QString{"color: %1; font-weight: 600;"}.arg(currentTheme().textMuted.name()));

    tree_ = new QTreeWidget{this};
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Property"), tr("Value")});
    tree_->setRootIsDecorated(true);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked |
                           QAbstractItemView::EditKeyPressed);
    // Interactive, sized by `resizeEvent`, rather than ResizeToContents.
    //
    // Fitting column 0 to its longest name sounds right and is not: the names
    // are long ("Linetype scale", "Lineweight"), the panel is narrow, and the
    // *value* column — the half the user is reading and editing — was pushed
    // off the right edge until the header itself read "Proper..." and "Valu".
    // The value column is the one that must always have room.
    tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree_->header()->setStretchLastSection(true);
    // Small, because this minimum applies to *every* section. Set to the name
    // column's minimum it silently applied to the value column too, so the two
    // together demanded more than the panel had once the vertical scroll bar
    // took its share -- which is what produced a horizontal scroll bar and
    // pushed "ByLayer" off the right edge. The real per-column minimums are
    // applied in `updateColumnWidths`, where they can be weighed against the
    // width actually available.
    tree_->header()->setMinimumSectionSize(24);
    // The two columns are fitted to the viewport by construction, so a
    // horizontal scroll bar can only mean the fit failed -- and it costs a row
    // of the height that the properties themselves need.
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Rows sit under a category, and the default indentation spends twenty
    // pixels of a narrow panel on saying so. The banded group heading already
    // says it.
    tree_->setIndentation(12);
    // Narrower than this and the panel is a column of ellipses.
    setMinimumWidth(kMinimumPanelWidth);

    connect(tree_, &QTreeWidget::itemChanged, this, &PropertiesPanel::onItemChanged);

    layout->addWidget(summary_);
    layout->addWidget(tree_, 1);
}

void PropertiesPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateColumnWidths();
}

void PropertiesPanel::updateColumnWidths() {
    // Names get exactly what they need — and no more than leaves the value
    // column legible.
    //
    // A share of the width was the obvious rule and it was wrong in both
    // directions: it wasted space on short names and truncated long ones in a
    // narrow panel. Fitting to the content and *capping* it is what keeps the
    // common case exact and the cramped case survivable, with the truncation
    // landing on the names, which a user can guess at, rather than on the
    // values, which they cannot.
    const int total = tree_->viewport()->width();
    if (total <= 0) {
        return;
    }
    // Qt will fit a column to its contents, but only in a resize mode that
    // then refuses to be capped. Asking in that mode, reading the answer, and
    // switching back is how to have both.
    QHeaderView* header = tree_->header();
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    const int wanted = std::max(tree_->columnWidth(0), kMinimumNameWidth);
    header->setSectionResizeMode(0, QHeaderView::Interactive);

    // The value column comes first in the argument: in a panel too narrow for
    // both, it is the half being read and edited, so the names give way.
    const int ceiling = std::max(total - kMinimumValueWidth, total / 3);
    tree_->setColumnWidth(0, std::clamp(wanted, std::min(kMinimumNameWidth, ceiling), ceiling));
}

void PropertiesPanel::setDocument(Document* document) {
    document_ = document;
    refresh();
}

void PropertiesPanel::refresh() {
    // The rebuild writes into the tree, and every write emits itemChanged.
    // Without this guard the panel would immediately write its own display
    // values back into the database as if the user had typed them.
    populating_ = true;
    tree_->clear();
    rows_.clear();

    if (document_ == nullptr || document_->selection().empty()) {
        summary_->setText(tr("No selection"));
        populating_ = false;
        // The empty panel still has headers to fit. This used to be covered by
        // the header's global minimum section size, which was doing the name
        // column's job by accident; with that minimum lowered so it stops
        // applying to the value column too, the widths have to be set here as
        // well or "Property" collapses to "Pro".
        updateColumnWidths();
        return;
    }

    const std::vector<Handle>& selection = document_->selection();
    rows_ = aggregateProperties(document_->db(), selection);

    if (selection.size() == 1) {
        const DbObject* object = document_->db().getObject(selection.front());
        summary_->setText(object == nullptr
                              ? tr("1 object")
                              : QString::fromStdString(object->objectType()));
    } else {
        summary_->setText(tr("%1 objects selected").arg(selection.size()));
    }

    // Grouped by category, in the order the categories first appear, so
    // "General" stays at the top where other CAD software users expect it.
    std::vector<QString> order;
    std::map<QString, QTreeWidgetItem*> groups;
    for (const AggregatedProperty& row : rows_) {
        const QString category = QString::fromStdString(row.definition->category);
        if (!groups.contains(category)) {
            auto* group = new QTreeWidgetItem{tree_, {category}};
            QFont font = group->font(0);
            font.setBold(true);
            group->setFont(0, font);
            group->setFirstColumnSpanned(true);
            group->setFlags(Qt::ItemIsEnabled);
            // A banded background, so a section reads as a section before any
            // of it is read. Bold text alone is not a boundary — it is one row
            // that happens to be heavier than its neighbours.
            const QBrush band{currentTheme().panelAlternate};
            group->setBackground(0, band);
            group->setBackground(1, band);
            groups[category] = group;
            order.push_back(category);
        }
    }

    for (std::size_t i = 0; i < rows_.size(); ++i) {
        buildRow(groups[QString::fromStdString(rows_[i].definition->category)], rows_[i]);
    }
    tree_->expandAll();
    populating_ = false;
    // The rows just changed, so the width they want has too.
    updateColumnWidths();
    // And again once the layout has settled. Straight after a rebuild Qt has
    // not yet decided whether the rows need a vertical scroll bar, so the
    // viewport still reports the width it had without one -- fifteen pixels
    // too many to divide between two columns. With no horizontal scroll bar to
    // take up the slack, the value column was not elided but clipped, which is
    // how "ByLayer" lost its last letters.
    QTimer::singleShot(0, this, &PropertiesPanel::updateColumnWidths);
}

void PropertiesPanel::buildRow(QTreeWidgetItem* parent, const AggregatedProperty& row) {
    const auto index = static_cast<int>(&row - rows_.data());

    auto* item = new QTreeWidgetItem{parent};
    item->setText(0, QString::fromStdString(row.definition->name));
    item->setText(1, displayText(row));
    item->setData(0, kRowIndexRole, index);

    if (row.definition->readOnly) {
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        // Only the *value* is muted, and only slightly. A read-only row is
        // information the user wants — an entity's handle, a computed area —
        // not a disabled control, and greying the label as well made half the
        // palette look switched off.
        item->setForeground(1, currentTheme().textMuted);
        item->setForeground(0, currentTheme().text);
        return;
    }

    if (row.definition->type == PropertyType::Bool) {
        // A checkbox rather than the words true and false: it is one click
        // instead of an edit, and the state is readable at a glance.
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setText(1, {});
        const bool checked = row.value && std::get_if<bool>(&*row.value) != nullptr &&
                             std::get<bool>(*row.value);
        item->setCheckState(1, row.varies ? Qt::PartiallyChecked
                                          : (checked ? Qt::Checked : Qt::Unchecked));
        return;
    }

    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);

    // A colour swatch beside the value, so the palette shows the colour rather
    // than only naming it.
    if (row.definition->type == PropertyType::Color && row.value) {
        if (const auto* color = std::get_if<Color>(&*row.value)) {
            QPixmap swatch{12, 12};
            swatch.fill(QColor::fromRgb(static_cast<QRgb>(color->toRgb())));
            item->setIcon(1, QIcon{swatch});
        }
    }
}

QString PropertiesPanel::displayText(const AggregatedProperty& row) const {
    if (row.varies || !row.value) {
        return kVaries;
    }
    const PropertyValue& value = *row.value;

    switch (row.definition->type) {
        case PropertyType::Angle:
            // Radians inside, degrees in front of the user. This is the
            // boundary, and the only place the conversion happens.
            if (const auto* number = std::get_if<double>(&value)) {
                return formatNumber(radToDeg(*number), 4) + QStringLiteral("°");
            }
            break;

        case PropertyType::Enumeration: {
            const auto* raw = std::get_if<std::int64_t>(&value);
            if (raw != nullptr) {
                for (const PropertyOption& option : row.definition->options) {
                    if (option.value == *raw) {
                        return QString::fromStdString(option.label);
                    }
                }
                return QString::number(*raw);
            }
            break;
        }

        case PropertyType::Reference:
            if (const auto* handle = std::get_if<Handle>(&value)) {
                return referenceName(*row.definition, *handle);
            }
            break;

        case PropertyType::Color:
            if (const auto* color = std::get_if<Color>(&value)) {
                return colorText(*color);
            }
            break;

        case PropertyType::Point:
            if (const auto* point = std::get_if<Vec3>(&value)) {
                return formatPoint(*point);
            }
            break;

        default:
            break;
    }

    if (const auto* text = std::get_if<std::string>(&value)) {
        return QString::fromStdString(*text);
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return formatNumber(*number);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return QString::number(*integer);
    }
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag ? tr("Yes") : tr("No");
    }
    return {};
}

const std::vector<Handle>* PropertiesPanel::referenceCandidates(
    const PropertyDefinition& definition) const {
    if (document_ == nullptr) {
        return nullptr;
    }
    const Database& db = document_->db();
    switch (definition.referenceTable) {
        case SymbolTableKind::Layers: return &db.layers().handles();
        case SymbolTableKind::Linetypes: return &db.linetypes().handles();
        case SymbolTableKind::TextStyles: return &db.textStyles().handles();
        case SymbolTableKind::DimStyles: return &db.dimStyles().handles();
        case SymbolTableKind::Blocks: return &db.blocks().handles();
        case SymbolTableKind::None:
        default: return nullptr;
    }
}

QString PropertiesPanel::referenceName(const PropertyDefinition& definition,
                                       Handle handle) const {
    if (document_ == nullptr) {
        return {};
    }
    if (handle.isNull()) {
        // A null linetype handle means ByLayer, which is a meaning rather than
        // an absence and has to read as one.
        return definition.referenceTable == SymbolTableKind::Linetypes ? tr("ByLayer")
                                                                       : QString{};
    }
    const auto* record = document_->db().get<SymbolTableRecord>(handle);
    return record == nullptr ? tr("(missing)") : QString::fromStdString(record->name);
}

std::optional<PropertyValue> PropertiesPanel::parseCell(const PropertyDefinition& definition,
                                                        const QString& text) const {
    switch (definition.type) {
        case PropertyType::Bool: {
            const QString lowered = text.trimmed().toLower();
            return PropertyValue{lowered == "yes" || lowered == "true" || lowered == "1"};
        }

        case PropertyType::Integer: {
            bool ok = false;
            const auto value = text.trimmed().toLongLong(&ok);
            return ok ? std::optional<PropertyValue>{value} : std::nullopt;
        }

        case PropertyType::Number:
        case PropertyType::Distance: {
            bool ok = false;
            const double value = text.trimmed().toDouble(&ok);
            return ok ? std::optional<PropertyValue>{value} : std::nullopt;
        }

        case PropertyType::Angle: {
            QString body = text.trimmed();
            body.remove(QStringLiteral("°"));
            bool ok = false;
            const double degrees = body.trimmed().toDouble(&ok);
            return ok ? std::optional<PropertyValue>{degToRad(degrees)} : std::nullopt;
        }

        case PropertyType::Enumeration: {
            const QString wanted = text.trimmed();
            for (const PropertyOption& option : definition.options) {
                if (wanted.compare(QString::fromStdString(option.label), Qt::CaseInsensitive) ==
                    0) {
                    return PropertyValue{option.value};
                }
            }
            // Falling back to the raw number lets a value be typed when the
            // label is awkward, and keeps the cell usable if a label changes.
            bool ok = false;
            const auto raw = wanted.toLongLong(&ok);
            return ok ? std::optional<PropertyValue>{raw} : std::nullopt;
        }

        case PropertyType::Reference: {
            const QString wanted = text.trimmed();
            if (wanted.isEmpty() ||
                (definition.referenceTable == SymbolTableKind::Linetypes &&
                 wanted.compare("ByLayer", Qt::CaseInsensitive) == 0)) {
                return PropertyValue{Handle::null()};
            }
            const std::vector<Handle>* candidates = referenceCandidates(definition);
            if (candidates == nullptr) {
                return std::nullopt;
            }
            for (const Handle handle : *candidates) {
                const auto* record = document_->db().get<SymbolTableRecord>(handle);
                if (record != nullptr &&
                    wanted.compare(QString::fromStdString(record->name), Qt::CaseInsensitive) ==
                        0) {
                    return PropertyValue{handle};
                }
            }
            // A name that matches nothing is rejected rather than creating a
            // record: a typo in a palette should not add a layer.
            return std::nullopt;
        }

        case PropertyType::Color: {
            const auto color = parseColor(text);
            return color ? std::optional<PropertyValue>{*color} : std::nullopt;
        }

        case PropertyType::Point: {
            const auto point = parsePoint(text);
            return point ? std::optional<PropertyValue>{*point} : std::nullopt;
        }

        case PropertyType::Text:
        default:
            return PropertyValue{text.toStdString()};
    }
}

void PropertiesPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (populating_ || document_ == nullptr || item == nullptr || column != 1) {
        return;
    }
    const QVariant index = item->data(0, kRowIndexRole);
    if (!index.isValid()) {
        return;
    }
    const auto row = static_cast<std::size_t>(index.toInt());
    if (row >= rows_.size()) {
        return;
    }
    const PropertyDefinition& definition = *rows_[row].definition;

    std::optional<PropertyValue> value;
    if (definition.type == PropertyType::Bool) {
        value = PropertyValue{item->checkState(1) == Qt::Checked};
    } else {
        const QString text = item->text(1);
        if (text == kVaries) {
            // The user opened the cell and closed it again without changing
            // anything; writing *VARIES* to every object would be a disaster.
            return;
        }
        value = parseCell(definition, text);
    }

    if (!value) {
        // Unparseable: put the old text back rather than leaving the cell
        // showing something the drawing does not contain.
        refresh();
        return;
    }
    applyEdit(definition, *value);
}

void PropertiesPanel::applyEdit(const PropertyDefinition& definition,
                                const PropertyValue& value) {
    const std::vector<Handle> selection = document_->selection();
    if (selection.empty()) {
        return;
    }

    std::size_t changed = 0;
    Transaction& transaction = document_->transactions().begin(definition.name);
    try {
        changed = applyProperty(transaction, selection, definition.id, value);
    } catch (const std::exception&) {
        document_->transactions().abort(transaction);
        refresh();
        return;
    }

    if (changed == 0) {
        // Nothing actually moved — every object was already at this value, or
        // all of them are locked. Committing would push an empty undo entry.
        document_->transactions().abort(transaction);
    } else {
        document_->transactions().commit(transaction);
    }

    document_->reindex();
    refresh();
    Q_EMIT propertyChanged();
}

}  // namespace cad::app
