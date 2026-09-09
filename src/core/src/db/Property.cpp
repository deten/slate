#include "cad/db/Property.hpp"

#include <algorithm>
#include <cmath>

#include "cad/db/ConstructionLineEntity.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/OleFrameEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/UnderlayEntity.hpp"
#include "cad/db/Transaction.hpp"

namespace cad {
namespace {

// --------------------------------------------------------------- conversions

[[nodiscard]] PropertyValue box(bool v) { return v; }
[[nodiscard]] PropertyValue box(double v) { return v; }
[[nodiscard]] PropertyValue box(int v) { return static_cast<std::int64_t>(v); }
[[nodiscard]] PropertyValue box(std::int16_t v) { return static_cast<std::int64_t>(v); }
[[nodiscard]] PropertyValue box(std::size_t v) { return static_cast<std::int64_t>(v); }
[[nodiscard]] PropertyValue box(const std::string& v) { return v; }
[[nodiscard]] PropertyValue box(const Color& v) { return v; }
[[nodiscard]] PropertyValue box(Handle v) { return v; }
[[nodiscard]] PropertyValue box(const Vec3& v) { return v; }

void unbox(bool& target, const PropertyValue& value) {
    if (const auto* v = std::get_if<bool>(&value)) { target = *v; }
}
void unbox(double& target, const PropertyValue& value) {
    if (const auto* v = std::get_if<double>(&value)) { target = *v; }
    else if (const auto* i = std::get_if<std::int64_t>(&value)) {
        // A palette that hands back a whole number as an integer must not
        // silently fail to set a double field.
        target = static_cast<double>(*i);
    }
}
void unbox(int& target, const PropertyValue& value) {
    if (const auto* v = std::get_if<std::int64_t>(&value)) { target = static_cast<int>(*v); }
}
void unbox(std::int16_t& target, const PropertyValue& value) {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
        target = static_cast<std::int16_t>(*v);
    }
}
void unbox(std::string& target, const PropertyValue& value) {
    if (const auto* v = std::get_if<std::string>(&value)) { target = *v; }
}
void unbox(Color& target, const PropertyValue& value) {
    if (const auto* v = std::get_if<Color>(&value)) { target = *v; }
}
void unbox(Handle& target, const PropertyValue& value) {
    if (const auto* v = std::get_if<Handle>(&value)) { target = *v; }
}
void unbox(Vec3& target, const PropertyValue& value) {
    if (const auto* v = std::get_if<Vec3>(&value)) { target = *v; }
}

// ------------------------------------------------------------- definitions

/// A property backed directly by a member.
///
/// The dynamic_cast is a guard rather than a dispatch: the registry only offers
/// a definition for the type it was registered against, so a mismatch means a
/// programming error somewhere else, and returning a default beats undefined
/// behaviour.
template <typename T, typename M>
[[nodiscard]] PropertyDefinition member(std::string id, std::string name, std::string category,
                                        PropertyType type, M T::*field) {
    PropertyDefinition property;
    property.id = std::move(id);
    property.name = std::move(name);
    property.category = std::move(category);
    property.type = type;
    property.get = [field](const DbObject& object) -> PropertyValue {
        const auto* self = dynamic_cast<const T*>(&object);
        return self == nullptr ? PropertyValue{} : box(self->*field);
    };
    property.set = [field](DbObject& object, const PropertyValue& value) {
        if (auto* self = dynamic_cast<T*>(&object)) {
            unbox(self->*field, value);
        }
    };
    return property;
}

/// A read-only property derived from the object.
template <typename T, typename F>
[[nodiscard]] PropertyDefinition computed(std::string id, std::string name,
                                          std::string category, PropertyType type, F compute) {
    PropertyDefinition property;
    property.id = std::move(id);
    property.name = std::move(name);
    property.category = std::move(category);
    property.type = type;
    property.readOnly = true;
    property.get = [compute](const DbObject& object) -> PropertyValue {
        const auto* self = dynamic_cast<const T*>(&object);
        return self == nullptr ? PropertyValue{} : box(compute(*self));
    };
    return property;
}

/// A property with its own getter and setter, for values that are not a member.
template <typename T, typename G, typename S>
[[nodiscard]] PropertyDefinition derived(std::string id, std::string name, std::string category,
                                         PropertyType type, G getter, S setter) {
    PropertyDefinition property;
    property.id = std::move(id);
    property.name = std::move(name);
    property.category = std::move(category);
    property.type = type;
    property.get = [getter](const DbObject& object) -> PropertyValue {
        const auto* self = dynamic_cast<const T*>(&object);
        return self == nullptr ? PropertyValue{} : box(getter(*self));
    };
    property.set = [setter](DbObject& object, const PropertyValue& value) {
        if (auto* self = dynamic_cast<T*>(&object)) {
            setter(*self, value);
        }
    };
    return property;
}

[[nodiscard]] PropertyDefinition reference(std::string id, std::string name,
                                           std::string category, SymbolTableKind table,
                                           Handle Entity::*field) {
    PropertyDefinition property =
        member<Entity, Handle>(std::move(id), std::move(name), std::move(category),
                               PropertyType::Reference, field);
    property.referenceTable = table;
    return property;
}

/// The lineweight choices other CAD software offers, sentinels first.
[[nodiscard]] std::vector<PropertyOption> lineweightOptions() {
    std::vector<PropertyOption> options{
        {toRaw(Lineweight::ByLayer), "ByLayer"},
        {toRaw(Lineweight::ByBlock), "ByBlock"},
        {toRaw(Lineweight::Default), "Default"},
    };
    for (const std::int16_t weight : standardLineweights()) {
        // Stored in hundredths of a millimetre; shown in millimetres, because
        // that is how lineweights are specified on a drawing sheet.
        const double millimetres = static_cast<double>(weight) / 100.0;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f mm", millimetres);
        options.push_back({weight, buffer});
    }
    return options;
}

// --------------------------------------------------------------- built-ins

[[nodiscard]] std::vector<PropertyDefinition> commonEntityProperties() {
    std::vector<PropertyDefinition> properties;

    properties.push_back(computed<DbObject>("general.type", "Type", "General",
                                            PropertyType::Text,
                                            [](const DbObject& o) { return o.objectType(); }));
    properties.push_back(computed<DbObject>(
        "general.handle", "Handle", "General", PropertyType::Text,
        [](const DbObject& o) { return o.handle.toString(); }));

    properties.push_back(reference("general.layer", "Layer", "General", SymbolTableKind::Layers,
                                   &Entity::layerHandle));
    properties.push_back(member<Entity, Color>("general.color", "Colour", "General",
                                               PropertyType::Color, &Entity::color));
    properties.push_back(reference("general.linetype", "Linetype", "General",
                                   SymbolTableKind::Linetypes, &Entity::linetypeHandle));
    properties.push_back(member<Entity, double>("general.linetypeScale", "Linetype scale",
                                                "General", PropertyType::Number,
                                                &Entity::linetypeScale));

    PropertyDefinition lineweight = member<Entity, std::int16_t>(
        "general.lineweight", "Lineweight", "General", PropertyType::Enumeration,
        &Entity::lineweight);
    lineweight.options = lineweightOptions();
    properties.push_back(std::move(lineweight));

    properties.push_back(member<Entity, bool>("general.visible", "Visible", "General",
                                              PropertyType::Bool, &Entity::visible));
    return properties;
}

void registerCurveEntities(PropertyRegistry& registry) {
    registry.registerFor(
        "Line",
        {
            member<LineEntity, Vec3>("geometry.start", "Start", "Geometry", PropertyType::Point,
                                     &LineEntity::start),
            member<LineEntity, Vec3>("geometry.end", "End", "Geometry", PropertyType::Point,
                                     &LineEntity::end),
            computed<LineEntity>("geometry.length", "Length", "Geometry",
                                 PropertyType::Distance,
                                 [](const LineEntity& e) { return e.start.distanceTo(e.end); }),
            computed<LineEntity>("geometry.angle", "Angle", "Geometry", PropertyType::Angle,
                                 [](const LineEntity& e) { return (e.end - e.start).angle2(); }),
        });

    for (const char* kind : {"Xline", "Ray"}) {
        registry.registerFor(
            kind,
            {
                member<ConstructionLineEntity, Vec3>("geometry.base", "Base point", "Geometry",
                                                     PropertyType::Point,
                                                     &ConstructionLineEntity::base),
                member<ConstructionLineEntity, Vec3>("geometry.direction", "Direction",
                                                     "Geometry", PropertyType::Point,
                                                     &ConstructionLineEntity::direction),
                computed<ConstructionLineEntity>(
                    "geometry.angle", "Angle", "Geometry", PropertyType::Angle,
                    [](const ConstructionLineEntity& e) { return e.direction.angle2(); }),
            });
    }

    registry.registerFor(
        "Circle",
        {
            member<CircleEntity, Vec3>("geometry.center", "Centre", "Geometry",
                                       PropertyType::Point, &CircleEntity::center),
            member<CircleEntity, double>("geometry.radius", "Radius", "Geometry",
                                         PropertyType::Distance, &CircleEntity::radius),
            // Editable, because a drafter given a diameter should not have to
            // halve it in their head.
            derived<CircleEntity>(
                "geometry.diameter", "Diameter", "Geometry", PropertyType::Distance,
                [](const CircleEntity& e) { return e.radius * 2.0; },
                [](CircleEntity& e, const PropertyValue& v) {
                    double diameter = e.radius * 2.0;
                    unbox(diameter, v);
                    if (diameter > 0.0) { e.radius = diameter / 2.0; }
                }),
            computed<CircleEntity>("geometry.circumference", "Circumference", "Geometry",
                                   PropertyType::Distance,
                                   [](const CircleEntity& e) { return kTau * e.radius; }),
            computed<CircleEntity>("geometry.area", "Area", "Geometry", PropertyType::Number,
                                   [](const CircleEntity& e) {
                                       return kPi * e.radius * e.radius;
                                   }),
        });

    registry.registerFor(
        "Arc",
        {
            member<ArcEntity, Vec3>("geometry.center", "Centre", "Geometry", PropertyType::Point,
                                    &ArcEntity::center),
            member<ArcEntity, double>("geometry.radius", "Radius", "Geometry",
                                      PropertyType::Distance, &ArcEntity::radius),
            member<ArcEntity, double>("geometry.startAngle", "Start angle", "Geometry",
                                      PropertyType::Angle, &ArcEntity::startAngle),
            member<ArcEntity, double>("geometry.endAngle", "End angle", "Geometry",
                                      PropertyType::Angle, &ArcEntity::endAngle),
            computed<ArcEntity>("geometry.totalAngle", "Total angle", "Geometry",
                                PropertyType::Angle,
                                [](const ArcEntity& e) {
                                    return normalizeAngle(e.endAngle - e.startAngle);
                                }),
            computed<ArcEntity>("geometry.length", "Arc length", "Geometry",
                                PropertyType::Distance,
                                [](const ArcEntity& e) { return e.geometry().length(); }),
        });

    registry.registerFor(
        "Ellipse",
        {
            member<EllipseEntity, Vec3>("geometry.center", "Centre", "Geometry",
                                        PropertyType::Point, &EllipseEntity::center),
            member<EllipseEntity, Vec3>("geometry.majorAxis", "Major axis", "Geometry",
                                        PropertyType::Point, &EllipseEntity::majorAxis),
            member<EllipseEntity, double>("geometry.ratio", "Minor/major ratio", "Geometry",
                                          PropertyType::Number, &EllipseEntity::ratio),
            member<EllipseEntity, double>("geometry.startParam", "Start parameter", "Geometry",
                                          PropertyType::Angle, &EllipseEntity::startParam),
            member<EllipseEntity, double>("geometry.endParam", "End parameter", "Geometry",
                                          PropertyType::Angle, &EllipseEntity::endParam),
        });

    registry.registerFor(
        "Polyline",
        {
            member<PolylineEntity, bool>("geometry.closed", "Closed", "Geometry",
                                         PropertyType::Bool, &PolylineEntity::closed),
            member<PolylineEntity, double>("geometry.constantWidth", "Global width", "Geometry",
                                           PropertyType::Distance,
                                           &PolylineEntity::constantWidth),
            computed<PolylineEntity>("geometry.vertexCount", "Vertices", "Geometry",
                                     PropertyType::Integer,
                                     [](const PolylineEntity& e) { return e.vertices.size(); }),
            computed<PolylineEntity>("geometry.length", "Length", "Geometry",
                                     PropertyType::Distance,
                                     [](const PolylineEntity& e) { return e.geometry().length(); }),
            computed<PolylineEntity>("geometry.area", "Area", "Geometry", PropertyType::Number,
                                     [](const PolylineEntity& e) {
                                         // An *open* polyline still encloses an
                                         // area, and other CAD software reports it: the
                                         // loop is closed with a straight run
                                         // from the last vertex to the first.
                                         //
                                         // This is not a detail. Area boundaries
                                         // in real drawings are routinely drawn
                                         // by clicking back onto the start point
                                         // rather than by using Close, so the
                                         // flag is off and the shape is shut.
                                         // Reporting zero for those makes every
                                         // area field on the sheet read 0.00.
                                         //
                                         // Signed area is negative for a
                                         // clockwise loop; the *area* is not.
                                         return std::abs(e.geometry().signedArea());
                                     }),
        });

    registry.registerFor("Point", {
                                      member<PointEntity, Vec3>("geometry.position", "Position",
                                                                "Geometry", PropertyType::Point,
                                                                &PointEntity::position),
                                  });
}

void registerTextEntities(PropertyRegistry& registry) {
    const std::vector<PropertyOption> horizontal{
        {0, "Left"}, {1, "Centre"}, {2, "Right"}, {3, "Aligned"}, {4, "Middle"}, {5, "Fit"}};
    const std::vector<PropertyOption> vertical{
        {0, "Baseline"}, {1, "Bottom"}, {2, "Middle"}, {3, "Top"}};

    auto textProperties = [&](bool includeTag) {
        std::vector<PropertyDefinition> properties{
            member<TextEntity, std::string>("text.contents", "Contents", "Text",
                                            PropertyType::Text, &TextEntity::contents),
            member<TextEntity, Vec3>("geometry.position", "Position", "Geometry",
                                     PropertyType::Point, &TextEntity::position),
            member<TextEntity, double>("text.height", "Height", "Text", PropertyType::Distance,
                                       &TextEntity::height),
            member<TextEntity, double>("text.rotation", "Rotation", "Text", PropertyType::Angle,
                                       &TextEntity::rotation),
            member<TextEntity, double>("text.widthFactor", "Width factor", "Text",
                                       PropertyType::Number, &TextEntity::widthFactor),
            member<TextEntity, double>("text.obliqueAngle", "Obliquing", "Text",
                                       PropertyType::Angle, &TextEntity::obliqueAngle),
        };

        PropertyDefinition style =
            member<TextEntity, Handle>("text.style", "Style", "Text", PropertyType::Reference,
                                       &TextEntity::styleHandle);
        style.referenceTable = SymbolTableKind::TextStyles;
        properties.push_back(std::move(style));

        PropertyDefinition horizontalAlignment = derived<TextEntity>(
            "text.horizontal", "Justify (horizontal)", "Text", PropertyType::Enumeration,
            [](const TextEntity& e) { return static_cast<int>(e.horizontalAlignment); },
            [](TextEntity& e, const PropertyValue& v) {
                int raw = static_cast<int>(e.horizontalAlignment);
                unbox(raw, v);
                if (raw >= 0 && raw <= 5) {
                    e.horizontalAlignment = static_cast<TextHorizontalAlignment>(raw);
                }
            });
        horizontalAlignment.options = horizontal;
        properties.push_back(std::move(horizontalAlignment));

        PropertyDefinition verticalAlignment = derived<TextEntity>(
            "text.vertical", "Justify (vertical)", "Text", PropertyType::Enumeration,
            [](const TextEntity& e) { return static_cast<int>(e.verticalAlignment); },
            [](TextEntity& e, const PropertyValue& v) {
                int raw = static_cast<int>(e.verticalAlignment);
                unbox(raw, v);
                if (raw >= 0 && raw <= 3) {
                    e.verticalAlignment = static_cast<TextVerticalAlignment>(raw);
                }
            });
        verticalAlignment.options = vertical;
        properties.push_back(std::move(verticalAlignment));

        if (includeTag) {
            properties.push_back(member<AttributeEntity, std::string>(
                "text.tag", "Tag", "Text", PropertyType::Text, &AttributeEntity::tag));
        }
        return properties;
    };

    registry.registerFor("Text", textProperties(false));
    registry.registerFor("Attribute", textProperties(true));
    registry.registerFor("AttributeDefinition", textProperties(false));

    std::vector<PropertyDefinition> mtext{
        member<MTextEntity, std::string>("text.contents", "Contents", "Text", PropertyType::Text,
                                         &MTextEntity::contents),
        member<MTextEntity, Vec3>("geometry.position", "Position", "Geometry",
                                  PropertyType::Point, &MTextEntity::position),
        member<MTextEntity, double>("text.height", "Height", "Text", PropertyType::Distance,
                                    &MTextEntity::height),
        member<MTextEntity, double>("text.width", "Column width", "Text", PropertyType::Distance,
                                    &MTextEntity::width),
        member<MTextEntity, double>("text.rotation", "Rotation", "Text", PropertyType::Angle,
                                    &MTextEntity::rotation),
        member<MTextEntity, double>("text.lineSpacing", "Line spacing", "Text",
                                    PropertyType::Number, &MTextEntity::lineSpacingFactor),
        computed<MTextEntity>("text.plain", "Plain text", "Text", PropertyType::Text,
                              [](const MTextEntity& e) { return e.plainText(); }),
    };
    PropertyDefinition mtextStyle =
        member<MTextEntity, Handle>("text.style", "Style", "Text", PropertyType::Reference,
                                    &MTextEntity::styleHandle);
    mtextStyle.referenceTable = SymbolTableKind::TextStyles;
    mtext.push_back(std::move(mtextStyle));
    registry.registerFor("MText", std::move(mtext));
}

void registerInsert(PropertyRegistry& registry) {
    std::vector<PropertyDefinition> properties{
        member<InsertEntity, Vec3>("geometry.position", "Position", "Geometry",
                                   PropertyType::Point, &InsertEntity::position),
        member<InsertEntity, double>("geometry.rotation", "Rotation", "Geometry",
                                     PropertyType::Angle, &InsertEntity::rotation),
        derived<InsertEntity>(
            "geometry.scaleX", "Scale X", "Geometry", PropertyType::Number,
            [](const InsertEntity& e) { return e.scale.x; },
            [](InsertEntity& e, const PropertyValue& v) { unbox(e.scale.x, v); }),
        derived<InsertEntity>(
            "geometry.scaleY", "Scale Y", "Geometry", PropertyType::Number,
            [](const InsertEntity& e) { return e.scale.y; },
            [](InsertEntity& e, const PropertyValue& v) { unbox(e.scale.y, v); }),
        member<InsertEntity, int>("geometry.columns", "Columns", "Geometry",
                                  PropertyType::Integer, &InsertEntity::columnCount),
        member<InsertEntity, int>("geometry.rows", "Rows", "Geometry", PropertyType::Integer,
                                  &InsertEntity::rowCount),
        member<InsertEntity, double>("geometry.columnSpacing", "Column spacing", "Geometry",
                                     PropertyType::Distance, &InsertEntity::columnSpacing),
        member<InsertEntity, double>("geometry.rowSpacing", "Row spacing", "Geometry",
                                     PropertyType::Distance, &InsertEntity::rowSpacing),
    };
    PropertyDefinition block =
        member<InsertEntity, Handle>("general.block", "Block", "General",
                                     PropertyType::Reference, &InsertEntity::blockHandle);
    block.referenceTable = SymbolTableKind::Blocks;
    properties.push_back(std::move(block));
    registry.registerFor("Insert", std::move(properties));
}

void registerAnnotation(PropertyRegistry& registry) {
    std::vector<PropertyDefinition> dimension{
        computed<DimensionEntity>("dim.measurement", "Measurement", "Dimension",
                                  PropertyType::Distance,
                                  [](const DimensionEntity& e) { return e.measure(); }),
        member<DimensionEntity, std::string>("dim.textOverride", "Text override", "Dimension",
                                             PropertyType::Text,
                                             &DimensionEntity::textOverride),
        member<DimensionEntity, Vec3>("dim.textPosition", "Text position", "Dimension",
                                      PropertyType::Point, &DimensionEntity::textMidPoint),
        member<DimensionEntity, double>("dim.leaderLength", "Leader length", "Dimension",
                                        PropertyType::Distance, &DimensionEntity::leaderLength),
        member<DimensionEntity, bool>("dim.textMoved", "Text moved", "Dimension",
                                      PropertyType::Bool,
                                      &DimensionEntity::textPositionIsUserDefined),
    };

    PropertyDefinition dimType = computed<DimensionEntity>(
        "dim.type", "Dimension type", "Dimension", PropertyType::Enumeration,
        [](const DimensionEntity& e) { return static_cast<int>(e.type); });
    dimType.options = {{0, "Rotated"},      {1, "Aligned"},  {2, "Angular (2 line)"},
                       {3, "Diameter"},     {4, "Radius"},   {5, "Angular (3 point)"},
                       {6, "Ordinate"}};
    dimension.push_back(std::move(dimType));

    PropertyDefinition dimStyle =
        member<DimensionEntity, Handle>("dim.style", "Dimension style", "Dimension",
                                        PropertyType::Reference, &DimensionEntity::styleHandle);
    dimStyle.referenceTable = SymbolTableKind::DimStyles;
    dimension.push_back(std::move(dimStyle));
    registry.registerFor("Dimension", std::move(dimension));

    std::vector<PropertyDefinition> hatch{
        derived<HatchEntity>(
            "hatch.pattern", "Pattern", "Hatch", PropertyType::Text,
            [](const HatchEntity& e) { return e.pattern.name; },
            [](HatchEntity& e, const PropertyValue& v) {
                std::string name = e.pattern.name;
                unbox(name, v);
                if (name == e.pattern.name) { return; }
                // Changing the name has to change the definition too, or the
                // hatch keeps drawing the old pattern under a new label.
                if (const auto known = standardHatchPattern(name)) {
                    const double scale = e.pattern.scale;
                    const double angle = e.pattern.angle;
                    e.pattern = *known;
                    e.pattern.scale = scale;
                    e.pattern.angle = angle;
                }
            }),
        derived<HatchEntity>(
            "hatch.scale", "Scale", "Hatch", PropertyType::Number,
            [](const HatchEntity& e) { return e.pattern.scale; },
            [](HatchEntity& e, const PropertyValue& v) {
                double scale = e.pattern.scale;
                unbox(scale, v);
                if (scale > 0.0) { e.pattern.scale = scale; }
            }),
        derived<HatchEntity>(
            "hatch.angle", "Angle", "Hatch", PropertyType::Angle,
            [](const HatchEntity& e) { return e.pattern.angle; },
            [](HatchEntity& e, const PropertyValue& v) { unbox(e.pattern.angle, v); }),
        derived<HatchEntity>(
            "hatch.double", "Double", "Hatch", PropertyType::Bool,
            [](const HatchEntity& e) { return e.pattern.isDouble; },
            [](HatchEntity& e, const PropertyValue& v) { unbox(e.pattern.isDouble, v); }),
        computed<HatchEntity>("hatch.associative", "Associative", "Hatch", PropertyType::Bool,
                              [](const HatchEntity& e) { return e.isAssociative; }),
        computed<HatchEntity>("hatch.paths", "Boundaries", "Hatch", PropertyType::Integer,
                              [](const HatchEntity& e) { return e.paths.size(); }),
    };

    PropertyDefinition islandStyle = derived<HatchEntity>(
        "hatch.style", "Island detection", "Hatch", PropertyType::Enumeration,
        [](const HatchEntity& e) { return static_cast<int>(e.style); },
        [](HatchEntity& e, const PropertyValue& v) {
            int raw = static_cast<int>(e.style);
            unbox(raw, v);
            if (raw >= 0 && raw <= 2) { e.style = static_cast<HatchStyle>(raw); }
        });
    islandStyle.options = {{0, "Normal"}, {1, "Outer"}, {2, "Ignore"}};
    hatch.push_back(std::move(islandStyle));
    registry.registerFor("Hatch", std::move(hatch));
}

/// Images, underlays and OLE frames.
///
/// These are the entities whose content comes from somewhere else, and the
/// palette is where a user finds out *where*. A drawing showing a rectangle
/// with no picture in it is a question, and the answer is a file path.
void registerAttachments(PropertyRegistry& registry) {
    std::vector<PropertyDefinition> image{
        computed<RasterImageEntity>(
            "image.width", "Width", "Image", PropertyType::Number,
            [](const RasterImageEntity& e) { return e.uVector.length() * e.widthPixels; }),
        computed<RasterImageEntity>(
            "image.height", "Height", "Image", PropertyType::Number,
            [](const RasterImageEntity& e) { return e.vVector.length() * e.heightPixels; }),
        computed<RasterImageEntity>(
            "image.pixels", "Pixel size", "Image", PropertyType::Text,
            [](const RasterImageEntity& e) {
                return std::to_string(static_cast<long long>(e.widthPixels)) + " x " +
                       std::to_string(static_cast<long long>(e.heightPixels));
            }),
        derived<RasterImageEntity>(
            "image.show", "Show image", "Image", PropertyType::Bool,
            [](const RasterImageEntity& e) { return e.showImage; },
            [](RasterImageEntity& e, const PropertyValue& v) { unbox(e.showImage, v); }),
        derived<RasterImageEntity>(
            "image.brightness", "Brightness", "Image", PropertyType::Integer,
            [](const RasterImageEntity& e) { return e.brightness; },
            [](RasterImageEntity& e, const PropertyValue& v) {
                int value = e.brightness;
                unbox(value, v);
                e.brightness = std::clamp(value, 0, 100);
            }),
        derived<RasterImageEntity>(
            "image.contrast", "Contrast", "Image", PropertyType::Integer,
            [](const RasterImageEntity& e) { return e.contrast; },
            [](RasterImageEntity& e, const PropertyValue& v) {
                int value = e.contrast;
                unbox(value, v);
                e.contrast = std::clamp(value, 0, 100);
            }),
        derived<RasterImageEntity>(
            "image.fade", "Fade", "Image", PropertyType::Integer,
            [](const RasterImageEntity& e) { return e.fade; },
            [](RasterImageEntity& e, const PropertyValue& v) {
                int value = e.fade;
                unbox(value, v);
                e.fade = std::clamp(value, 0, 100);
            }),
        computed<RasterImageEntity>(
            "image.clipped", "Clipped", "Image", PropertyType::Bool,
            [](const RasterImageEntity& e) { return e.clippingEnabled; }),
    };
    registry.registerFor("Image", std::move(image));

    const auto underlayProperties = [] {
        return std::vector<PropertyDefinition>{
            member<UnderlayEntity, Vec3>("underlay.position", "Position", "Underlay",
                                         PropertyType::Point, &UnderlayEntity::position),
            member<UnderlayEntity, Vec3>("underlay.scale", "Scale", "Underlay",
                                         PropertyType::Point, &UnderlayEntity::scale),
            member<UnderlayEntity, double>("underlay.rotation", "Rotation", "Underlay",
                                           PropertyType::Angle, &UnderlayEntity::rotation),
            derived<UnderlayEntity>(
                "underlay.on", "Show underlay", "Underlay", PropertyType::Bool,
                [](const UnderlayEntity& e) { return e.underlayOn; },
                [](UnderlayEntity& e, const PropertyValue& v) { unbox(e.underlayOn, v); }),
            derived<UnderlayEntity>(
                "underlay.monochrome", "Monochrome", "Underlay", PropertyType::Bool,
                [](const UnderlayEntity& e) { return e.monochrome; },
                [](UnderlayEntity& e, const PropertyValue& v) { unbox(e.monochrome, v); }),
            derived<UnderlayEntity>(
                "underlay.contrast", "Contrast", "Underlay", PropertyType::Integer,
                [](const UnderlayEntity& e) { return e.contrast; },
                [](UnderlayEntity& e, const PropertyValue& v) {
                    int value = e.contrast;
                    unbox(value, v);
                    e.contrast = std::clamp(value, 0, 100);
                }),
            derived<UnderlayEntity>(
                "underlay.fade", "Fade", "Underlay", PropertyType::Integer,
                [](const UnderlayEntity& e) { return e.fade; },
                [](UnderlayEntity& e, const PropertyValue& v) {
                    int value = e.fade;
                    unbox(value, v);
                    e.fade = std::clamp(value, 0, 100);
                }),
            computed<UnderlayEntity>(
                "underlay.clipped", "Clipped", "Underlay", PropertyType::Bool,
                [](const UnderlayEntity& e) { return e.clippingEnabled; }),
        };
    };
    // One registration per kind, because the type is what the palette keys on
    // and a PDF and a DGN are not the same thing to the person looking at them.
    registry.registerFor("PdfUnderlay", underlayProperties());
    registry.registerFor("DwfUnderlay", underlayProperties());
    registry.registerFor("DgnUnderlay", underlayProperties());

    registry.registerFor(
        "OleFrame",
        {
            member<OleFrameEntity, Vec3>("ole.upperLeft", "Upper left", "OLE",
                                         PropertyType::Point, &OleFrameEntity::upperLeft),
            member<OleFrameEntity, Vec3>("ole.lowerRight", "Lower right", "OLE",
                                         PropertyType::Point, &OleFrameEntity::lowerRight),
        });
}

void registerLayer(PropertyRegistry& registry) {
    std::vector<PropertyDefinition> layer{
        member<Layer, std::string>("layer.name", "Name", "Layer", PropertyType::Text,
                                   &Layer::name),
        member<Layer, Color>("layer.color", "Colour", "Layer", PropertyType::Color,
                             &Layer::color),
        member<Layer, bool>("layer.off", "Off", "Layer", PropertyType::Bool, &Layer::isOff),
        member<Layer, bool>("layer.frozen", "Frozen", "Layer", PropertyType::Bool,
                            &Layer::isFrozen),
        member<Layer, bool>("layer.locked", "Locked", "Layer", PropertyType::Bool,
                            &Layer::isLocked),
        member<Layer, bool>("layer.plottable", "Plot", "Layer", PropertyType::Bool,
                            &Layer::isPlottable),
        member<Layer, std::string>("layer.description", "Description", "Layer",
                                   PropertyType::Text, &Layer::description),
    };

    PropertyDefinition linetype =
        member<Layer, Handle>("layer.linetype", "Linetype", "Layer", PropertyType::Reference,
                              &Layer::linetypeHandle);
    linetype.referenceTable = SymbolTableKind::Linetypes;
    layer.push_back(std::move(linetype));

    PropertyDefinition lineweight = member<Layer, std::int16_t>(
        "layer.lineweight", "Lineweight", "Layer", PropertyType::Enumeration, &Layer::lineweight);
    lineweight.options = lineweightOptions();
    layer.push_back(std::move(lineweight));

    registry.registerFor("Layer", std::move(layer));
}

}  // namespace

// -------------------------------------------------------------------- registry

const PropertyRegistry& PropertyRegistry::standard() {
    static const PropertyRegistry registry = [] {
        PropertyRegistry built;
        built.registerCommon(commonEntityProperties());
        registerCurveEntities(built);
        registerTextEntities(built);
        registerInsert(built);
        registerAnnotation(built);
        registerAttachments(built);
        registerLayer(built);
        return built;
    }();
    return registry;
}

void PropertyRegistry::registerFor(std::string objectType,
                                   std::vector<PropertyDefinition> properties) {
    std::vector<PropertyDefinition>& target = byType_[std::move(objectType)];
    target.insert(target.end(), std::make_move_iterator(properties.begin()),
                  std::make_move_iterator(properties.end()));
}

void PropertyRegistry::registerCommon(std::vector<PropertyDefinition> properties) {
    common_.insert(common_.end(), std::make_move_iterator(properties.begin()),
                   std::make_move_iterator(properties.end()));
}

std::vector<const PropertyDefinition*> PropertyRegistry::propertiesFor(
    const DbObject& object) const {
    std::vector<const PropertyDefinition*> result;

    // Common properties are entity-wide, so a symbol table record does not get
    // a Layer row pointing at itself.
    if (dynamic_cast<const Entity*>(&object) != nullptr) {
        for (const PropertyDefinition& property : common_) {
            result.push_back(&property);
        }
    }

    const auto it = byType_.find(object.objectType());
    if (it != byType_.end()) {
        for (const PropertyDefinition& property : it->second) {
            result.push_back(&property);
        }
    }
    return result;
}

const PropertyDefinition* PropertyRegistry::find(const DbObject& object,
                                                 const std::string& id) const {
    for (const PropertyDefinition* property : propertiesFor(object)) {
        if (property->id == id) {
            return property;
        }
    }
    return nullptr;
}

// ----------------------------------------------------------------- comparison

bool propertyValuesEqual(const PropertyValue& a, const PropertyValue& b) {
    if (a.index() != b.index()) {
        return false;
    }
    // Compared alternative by alternative rather than with the variant's own
    // `==`. Two of them need it: doubles compare by tolerance, so a palette
    // does not report a difference nobody can see and then rewrite both values
    // to "fix" it, and Colour carries a colour-book name that two otherwise
    // identical colours may spell differently.
    if (const auto* left = std::get_if<double>(&a)) {
        return std::abs(*left - std::get<double>(b)) <= 1e-9;
    }
    if (const auto* left = std::get_if<Color>(&a)) {
        return left->equals(std::get<Color>(b));
    }
    if (const auto* left = std::get_if<Vec3>(&a)) {
        return left->equals(std::get<Vec3>(b));
    }
    if (const auto* left = std::get_if<bool>(&a)) {
        return *left == std::get<bool>(b);
    }
    if (const auto* left = std::get_if<std::int64_t>(&a)) {
        return *left == std::get<std::int64_t>(b);
    }
    if (const auto* left = std::get_if<std::string>(&a)) {
        return *left == std::get<std::string>(b);
    }
    if (const auto* left = std::get_if<Handle>(&a)) {
        return *left == std::get<Handle>(b);
    }
    return false;
}

// ---------------------------------------------------------------- aggregation

std::vector<AggregatedProperty> aggregateProperties(const Database& db,
                                                    const std::vector<Handle>& handles,
                                                    const PropertyRegistry& registry) {
    std::vector<AggregatedProperty> rows;
    std::size_t objectCount = 0;

    for (const Handle handle : handles) {
        const DbObject* object = db.getObject(handle);
        if (object == nullptr || object->erased) {
            continue;
        }
        ++objectCount;

        for (const PropertyDefinition* definition : registry.propertiesFor(*object)) {
            const PropertyValue value = definition->get(*object);

            const auto existing = std::find_if(rows.begin(), rows.end(),
                                               [&](const AggregatedProperty& row) {
                                                   return row.definition->id == definition->id;
                                               });
            if (existing == rows.end()) {
                AggregatedProperty row;
                row.definition = definition;
                row.value = value;
                row.count = 1;
                rows.push_back(std::move(row));
                continue;
            }
            ++existing->count;
            if (existing->value && !propertyValuesEqual(*existing->value, value)) {
                existing->varies = true;
                existing->value.reset();
            }
        }
    }

    // A row that only some objects have is dropped. Showing it would invite an
    // edit that silently applied to part of the selection.
    std::erase_if(rows, [objectCount](const AggregatedProperty& row) {
        return row.count != objectCount;
    });
    return rows;
}

// ------------------------------------------------------------------ applying

std::size_t applyProperty(Transaction& transaction, const std::vector<Handle>& handles,
                          const std::string& id, const PropertyValue& value,
                          const PropertyRegistry& registry) {
    Database& db = transaction.database();
    std::size_t changed = 0;

    for (const Handle handle : handles) {
        DbObject* object = db.getObject(handle);
        if (object == nullptr || object->erased) {
            continue;
        }

        // A locked layer means "visible but not editable", and a properties
        // palette is exactly the back door that has to respect it.
        if (const auto* entity = dynamic_cast<const Entity*>(object)) {
            const auto* layer = db.get<Layer>(entity->layerHandle);
            if (layer != nullptr && layer->isLocked) {
                continue;
            }
        }

        const PropertyDefinition* definition = registry.find(*object, id);
        if (definition == nullptr || definition->readOnly || !definition->set) {
            continue;
        }
        // Setting a value it already has would push an undo entry describing
        // nothing.
        if (propertyValuesEqual(definition->get(*object), value)) {
            continue;
        }

        transaction.openForWrite(*object);
        definition->set(*object, value);
        db.notifyModified(handle);
        ++changed;
    }
    return changed;
}

}  // namespace cad
