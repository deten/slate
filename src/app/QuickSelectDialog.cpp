#include "QuickSelectDialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <algorithm>

#include "Document.hpp"
#include "LayerOrdering.hpp"
#include "cad/db/Layer.hpp"

namespace cad::app {
namespace {

constexpr int kPropertyIdRole = Qt::UserRole + 1;
constexpr int kPropertyTypeRole = Qt::UserRole + 2;

}  // namespace

QuickSelectDialog::QuickSelectDialog(Document& document, QWidget* parent)
    : QDialog{parent}, document_(document) {
    setWindowTitle(tr("Quick Select"));
    setMinimumWidth(420);

    auto* outer = new QVBoxLayout{this};
    auto* form = new QFormLayout;

    type_ = new QComboBox{this};
    type_->addItem(tr("All objects"), QString{});
    for (const std::string& name : typesInDrawing()) {
        type_->addItem(QString::fromStdString(name), QString::fromStdString(name));
    }
    form->addRow(tr("Object type"), type_);

    property_ = new QComboBox{this};
    form->addRow(tr("Property"), property_);

    comparison_ = new QComboBox{this};
    comparison_->addItem(tr("equals"), static_cast<int>(SelectionFilter::Comparison::Equals));
    comparison_->addItem(tr("does not equal"),
                         static_cast<int>(SelectionFilter::Comparison::NotEquals));
    comparison_->addItem(tr("greater than"),
                         static_cast<int>(SelectionFilter::Comparison::Greater));
    comparison_->addItem(tr("less than"), static_cast<int>(SelectionFilter::Comparison::Less));
    comparison_->addItem(tr("contains"),
                         static_cast<int>(SelectionFilter::Comparison::Contains));
    form->addRow(tr("Operator"), comparison_);

    value_ = new QLineEdit{this};
    valueChoice_ = new QComboBox{this};
    valueChoice_->setVisible(false);
    form->addRow(tr("Value"), value_);
    form->addRow(QString{}, valueChoice_);

    mode_ = new QComboBox{this};
    mode_->addItem(tr("Replace the selection"), static_cast<int>(SelectionFilter::Mode::Replace));
    mode_->addItem(tr("Add to the selection"), static_cast<int>(SelectionFilter::Mode::Add));
    mode_->addItem(tr("Remove from the selection"),
                   static_cast<int>(SelectionFilter::Mode::Remove));
    form->addRow(tr("Result"), mode_);

    outer->addLayout(form);

    preview_ = new QLabel{this};
    preview_->setWordWrap(true);
    outer->addWidget(preview_);

    auto* buttons = new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this};
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    connect(type_, &QComboBox::currentIndexChanged, this, [this] { refreshProperties(); });
    connect(property_, &QComboBox::currentIndexChanged, this, [this] { refreshValueEditor(); });
    connect(comparison_, &QComboBox::currentIndexChanged, this, [this] { refreshPreview(); });
    connect(value_, &QLineEdit::textChanged, this, [this] { refreshPreview(); });
    connect(valueChoice_, &QComboBox::currentIndexChanged, this, [this] { refreshPreview(); });

    refreshProperties();
}

std::vector<std::string> QuickSelectDialog::typesInDrawing() const {
    std::vector<std::string> types;
    for (const Entity* entity : document_.db().entitiesIn(document_.db().modelSpace())) {
        if (entity->erased) {
            continue;
        }
        const std::string type = entity->objectType();
        if (std::find(types.begin(), types.end(), type) == types.end()) {
            types.push_back(type);
        }
    }
    std::sort(types.begin(), types.end());
    return types;
}

void QuickSelectDialog::refreshProperties() {
    const QString previous = property_->currentData(kPropertyIdRole).toString();
    property_->clear();
    property_->addItem(tr("(any — match the type only)"), QString{});

    // Properties are collected from a real object of the chosen type, so the
    // list is what the drawing actually offers rather than a hard-coded set.
    // With "All objects" chosen, only the common entity properties make sense,
    // since a property one type has is not comparable across the rest.
    const QString wanted = type_->currentData().toString();
    const PropertyRegistry& registry = PropertyRegistry::standard();

    std::vector<const PropertyDefinition*> definitions;
    for (const Entity* entity : document_.db().entitiesIn(document_.db().modelSpace())) {
        if (entity->erased) {
            continue;
        }
        if (!wanted.isEmpty() && QString::fromStdString(entity->objectType()) != wanted) {
            continue;
        }
        definitions = registry.propertiesFor(*entity);
        break;
    }

    for (const PropertyDefinition* definition : definitions) {
        if (wanted.isEmpty() && definition->category != "General") {
            continue;
        }
        const int index = property_->count();
        property_->addItem(QString::fromStdString(definition->name));
        property_->setItemData(index, QString::fromStdString(definition->id), kPropertyIdRole);
        property_->setItemData(index, static_cast<int>(definition->type), kPropertyTypeRole);
    }

    // Keep the user's choice across a type change when it still exists.
    for (int i = 0; i < property_->count(); ++i) {
        if (property_->itemData(i, kPropertyIdRole).toString() == previous) {
            property_->setCurrentIndex(i);
            break;
        }
    }
    refreshValueEditor();
}

void QuickSelectDialog::refreshValueEditor() {
    const auto type =
        static_cast<PropertyType>(property_->currentData(kPropertyTypeRole).toInt());
    const bool hasProperty = !property_->currentData(kPropertyIdRole).toString().isEmpty();

    // A layer is chosen from the layers that exist, not typed. Typing a layer
    // name is how a query silently matches nothing because of a capital letter.
    const bool isLayer = hasProperty && type == PropertyType::Reference;
    valueChoice_->setVisible(isLayer);
    value_->setVisible(hasProperty && !isLayer);
    comparison_->setEnabled(hasProperty);

    if (isLayer) {
        valueChoice_->clear();
        for (const Handle handle : layersByName(document_.db())) {
            if (const auto* layer = document_.db().get<Layer>(handle); layer != nullptr) {
                valueChoice_->addItem(QString::fromStdString(layer->name),
                                      static_cast<qulonglong>(handle.value()));
            }
        }
    }
    refreshPreview();
}

void QuickSelectDialog::refreshPreview() {
    const std::vector<Handle> matches =
        findByFilter(document_.db(), document_.db().modelSpace(), filter());
    preview_->setText(tr("%n object(s) match", "", static_cast<int>(matches.size())));
}

SelectionFilter QuickSelectDialog::filter() const {
    SelectionFilter result;
    result.objectType = type_->currentData().toString().toStdString();
    result.property = property_->currentData(kPropertyIdRole).toString().toStdString();
    result.comparison =
        static_cast<SelectionFilter::Comparison>(comparison_->currentData().toInt());
    result.mode = static_cast<SelectionFilter::Mode>(mode_->currentData().toInt());

    if (!result.property.empty()) {
        const auto type =
            static_cast<PropertyType>(property_->currentData(kPropertyTypeRole).toInt());
        switch (type) {
            case PropertyType::Reference:
                result.value = Handle{valueChoice_->currentData().toULongLong()};
                break;
            case PropertyType::Bool:
                result.value = value_->text().trimmed().compare("true", Qt::CaseInsensitive) == 0;
                break;
            case PropertyType::Integer:
            case PropertyType::Enumeration:
                result.value = static_cast<std::int64_t>(value_->text().toLongLong());
                break;
            case PropertyType::Number:
            case PropertyType::Distance:
                result.value = value_->text().toDouble();
                break;
            case PropertyType::Angle:
                // Typed in degrees and stored in radians, as everywhere else.
                result.value = degToRad(value_->text().toDouble());
                break;
            case PropertyType::Color:
                result.value = Color::fromIndex(static_cast<std::int16_t>(value_->text().toInt()));
                break;
            case PropertyType::Text:
            case PropertyType::Point:
                result.value = value_->text().toStdString();
                break;
        }
    }
    return result;
}

}  // namespace cad::app
