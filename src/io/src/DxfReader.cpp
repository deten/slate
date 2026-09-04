#include "cad/io/DxfReader.hpp"

#include <algorithm>
#include <set>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/Dictionary.hpp"
#include "cad/db/FieldObject.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/FieldEvaluator.hpp"
#include "cad/db/MLeaderEntity.hpp"
#include "cad/db/TableEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/UnderlayEntity.hpp"

namespace cad::io {

bool DxfReadResult::succeeded() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const DxfDiagnostic& d) {
        return d.severity == DxfDiagnostic::Severity::Error;
    });
}

/// The reader's working state, kept out of the header so the section-parsing
/// details are not part of the public interface.
class DxfReader::Impl {
public:
    explicit Impl(const EntityReaderRegistry& registry)
        : registry_(registry),
          db_(std::make_unique<Database>()),
          // Constructed once and held for the reader's lifetime. Deferred
          // resolution lambdas capture this by reference and run after the
          // entity that created them has been read, so a per-entity context
          // would dangle by the time they fire.
          context_{*db_, diagnostics_, deferred_, blockNamesToHandles_} {}

    DxfReadResult read(std::string_view text) {
        reserveFileHandles(text);

        DxfTokenizer tokenizer{text};
        GroupCursor cursor{tokenizer};

        db_->bulkLoad([&] {
            readSections(cursor);
            // Only *after* the file's own records are in, so the fallbacks
            // cannot claim handles the file uses for something else.
            ensureMinimumTables();

            // Name-based references resolve only once every object exists.
            for (const auto& action : deferred_) {
                try {
                    action();
                } catch (const std::exception& error) {
                    diagnostics_.push_back(
                        {DxfDiagnostic::Severity::Warning,
                         std::string{"Deferred reference resolution failed: "} + error.what(),
                         0});
                }
            }
            // Last, because it walks dictionaries that must all be present.
            linkFieldsToTheirObjects();
            // And then the table cells, whose values are composed rather than
            // stored: a schedule read without this is an empty grid.
            refreshTableCells(*db_);
        });

        const auto& tokenizerDiagnostics = tokenizer.diagnostics();
        diagnostics_.insert(diagnostics_.end(), tokenizerDiagnostics.begin(),
                            tokenizerDiagnostics.end());

        DxfReadResult result;
        result.database = std::move(db_);
        result.diagnostics = std::move(diagnostics_);
        result.version = version_;
        result.versionName = std::string{dxfVersionName(version_)};
        result.entityCount = entityCount_;
        return result;
    }

private:
    /// Reserve every handle the file uses, before parsing begins.
    ///
    /// Records created on demand during parsing — the layer an entity names but
    /// the LAYER table omitted, for instance — need a handle immediately, and
    /// any handle allocated then may be one the file uses for an object *later*
    /// in the stream. The collision only surfaces when that object arrives,
    /// which aborts the load of an otherwise readable drawing.
    ///
    /// A cheap pre-pass over the handle codes makes the allocator's range
    /// disjoint from the file's, so on-demand creation is always safe. The
    /// header's `$HANDSEED` is not sufficient: real files disagree with it, and
    /// trusting it is what the allocator's `observe` exists to compensate for.
    void reserveFileHandles(std::string_view text) {
        DxfTokenizer scanner{text};
        while (const auto group = scanner.next()) {
            // 5 is the object handle; 105 the DIMSTYLE handle, which is the one
            // exception to the rule and shares the same numbering space.
            if (group->code != 5 && group->code != 105) {
                continue;
            }
            if (const auto handle = Handle::tryParse(group->asString())) {
                db_->observeHandle(*handle);
            }
        }
    }

    /// Model space, creating it if the file never defined one.
    ///
    /// Called lazily rather than up front: creating it before parsing would
    /// consume a low handle that the file may legitimately use for one of its
    /// own records, and the resulting collision aborts the load.
    [[nodiscard]] BlockTableRecord& ensureModelSpace() {
        if (BlockTableRecord* existing = db_->findBlock(kModelSpace)) {
            return *existing;
        }
        auto model = std::make_unique<BlockTableRecord>();
        model->name = kModelSpace;
        return db_->addBlock(std::move(model));
    }

    /// Create the records a valid drawing must have.
    ///
    /// Runs *after* the file has been read: the file normally defines its own
    /// and those take precedence, handles included. This only guarantees that a
    /// malformed file — one missing its LAYER table entirely, which does occur
    /// — still produces a usable database.
    void ensureMinimumTables() {
        if (db_->findLinetype(kLinetypeContinuous) == nullptr) {
            auto linetype = std::make_unique<Linetype>();
            linetype->name = kLinetypeContinuous;
            linetype->description = "Solid line";
            db_->addLinetype(std::move(linetype));
        }
        if (db_->findTextStyle(kTextStyleStandard) == nullptr) {
            auto style = std::make_unique<TextStyle>();
            style->name = kTextStyleStandard;
            db_->addTextStyle(std::move(style));
        }
        if (db_->findLayer(kLayerZero) == nullptr) {
            auto layer = std::make_unique<Layer>();
            layer->name = kLayerZero;
            layer->color = Color::fromIndex(7);
            db_->addLayer(std::move(layer));
        }
        if (db_->findBlock(kModelSpace) == nullptr) {
            auto model = std::make_unique<BlockTableRecord>();
            model->name = kModelSpace;
            db_->addBlock(std::move(model));
        }
    }

    void readSections(GroupCursor& cursor) {
        for (;;) {
            const auto group = cursor.next();
            if (!group) {
                return;
            }
            if (group->code != 0) {
                continue;
            }

            const std::string value = normalizeSymbolName(group->asString());
            if (value == "EOF") {
                return;
            }
            if (value != "SECTION") {
                continue;
            }

            const auto nameGroup = cursor.next();
            if (!nameGroup) {
                return;
            }
            if (nameGroup->code != 2) {
                diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                        "SECTION without a name; skipping", nameGroup->line});
                continue;
            }

            const std::string sectionName = normalizeSymbolName(nameGroup->asString());
            if (sectionName == section::kHeader) {
                readHeader(cursor);
            } else if (sectionName == section::kTables) {
                readTables(cursor);
            } else if (sectionName == section::kBlocks) {
                readBlocks(cursor);
            } else if (sectionName == section::kEntities) {
                readEntities(cursor);
            } else if (sectionName == "OBJECTS") {
                readObjects(cursor);
            } else {
                // CLASSES, THUMBNAILIMAGE and ACDSDATA hold nothing this reader
                // interprets — which is a reason not to read them, and no
                // reason at all to delete them. They are kept verbatim and
                // written back where they came from.
                //
                // CLASSES is the one that matters most: it tells other CAD software how to
                // construct the object types a drawing uses that are not in the
                // base set, and this drawing's has 42 entries covering its
                // tables, its raster image, its section and detail view styles.
                // Saving without it turns every one of those into a proxy.
                captureSection(sectionName, cursor);
            }
        }
    }

    /// Collect a whole section verbatim, stopping at its ENDSEC.
    void captureSection(const std::string& name, GroupCursor& cursor) {
        PreservedSection section;
        section.name = name;
        for (;;) {
            const auto group = cursor.next();
            if (!group) {
                break;
            }
            if (group->code == 0 && normalizeSymbolName(group->asString()) == "ENDSEC") {
                break;
            }
            section.groups.push_back({group->code, toPreservedValue(*group)});
        }
        if (!section.groups.empty()) {
            db_->preservedSections.push_back(std::move(section));
        }
    }

    /// The variant the database stores a raw group's value in.
    ///
    /// Numbers stay numbers and integers stay integers, so a re-emitted section
    /// is formatted the way its codes call for rather than as whatever text the
    /// tokenizer happened to see.
    [[nodiscard]] static XDataValue toPreservedValue(const DxfGroup& group) {
        if (const auto* text = std::get_if<std::string>(&group.value)) {
            return *text;
        }
        if (const auto* number = std::get_if<double>(&group.value)) {
            return *number;
        }
        if (const auto* integer = std::get_if<std::int64_t>(&group.value)) {
            return *integer;
        }
        return group.asString();
    }

    // ------------------------------------------------------------- header

    void readHeader(GroupCursor& cursor) {
        std::string currentName;
        std::vector<DxfGroup> pending;

        const auto flush = [&] {
            if (!currentName.empty()) {
                applyHeaderVariable(currentName, pending);
            }
            currentName.clear();
            pending.clear();
        };

        for (;;) {
            const auto group = cursor.next();
            if (!group) {
                flush();
                return;
            }
            if (group->code == 0 && normalizeSymbolName(group->asString()) == "ENDSEC") {
                flush();
                return;
            }
            if (group->code == 9) {
                flush();
                currentName = normalizeSymbolName(group->asString());
                continue;
            }
            if (!currentName.empty()) {
                pending.push_back(*group);
            }
        }
    }

    void applyHeaderVariable(const std::string& name, const std::vector<DxfGroup>& groups) {
        const auto numberAt = [&](double fallback) {
            return groups.empty() ? fallback : groups.front().asDouble();
        };
        const auto textAt = [&](std::string fallback) {
            return groups.empty() ? std::move(fallback) : groups.front().asString();
        };
        const auto pointAt = [&] {
            Vec3 result;
            for (const DxfGroup& group : groups) {
                if (group.code == 10) result.x = group.asDouble();
                if (group.code == 20) result.y = group.asDouble();
                if (group.code == 30) result.z = group.asDouble();
            }
            return result;
        };

        DrawingSettings& settings = db_->settings;

        if (name == "$ACADVER") {
            version_ = textAt({});
            db_->sourceFormat = version_;
        } else if (name == "$INSUNITS") {
            settings.insertionUnits = static_cast<DrawingUnits>(
                static_cast<std::uint8_t>(numberAt(4.0)));
        } else if (name == "$LTSCALE") {
            settings.globalLinetypeScale = numberAt(1.0);
        } else if (name == "$PSLTSCALE") {
            settings.paperSpaceLinetypeScale = numberAt(1.0);
        } else if (name == "$LUPREC") {
            settings.linearPrecision = static_cast<int>(numberAt(4.0));
        } else if (name == "$AUPREC") {
            settings.angularPrecision = static_cast<int>(numberAt(2.0));
        } else if (name == "$ANGBASE") {
            settings.angleBase = degToRad(numberAt(0.0));
        } else if (name == "$ANGDIR") {
            settings.angleClockwise = numberAt(0.0) != 0.0;
        } else if (name == "$LWDISPLAY") {
            settings.displayLineweights = numberAt(0.0) != 0.0;
        } else if (name == "$PDMODE") {
            settings.pointDisplayMode = static_cast<int>(numberAt(0.0));
        } else if (name == "$PDSIZE") {
            settings.pointDisplaySize = numberAt(0.0);
        } else if (name == "$LIMMIN") {
            settings.limitsMin = pointAt();
        } else if (name == "$LIMMAX") {
            settings.limitsMax = pointAt();
        } else if (name == "$INSBASE") {
            settings.insertionBase = pointAt();
        } else if (name == "$GRIDUNIT") {
            settings.gridSpacing = pointAt();
        } else if (name == "$SNAPUNIT") {
            settings.snapSpacing = pointAt();
        } else if (name == "$ORTHOMODE") {
            settings.orthoEnabled = numberAt(0.0) != 0.0;
        } else if (name == "$OSMODE") {
            settings.osnapModes = static_cast<int>(numberAt(0.0));
        } else if (name == "$CLAYER") {
            settings.currentLayerName = textAt("0");
        } else if (name == "$TEXTSTYLE") {
            settings.currentTextStyleName = textAt("Standard");
        } else if (name == "$CELTYPE") {
            settings.currentLinetypeName = textAt("ByLayer");
        } else if (name == "$DIMSTYLE") {
            settings.currentDimStyleName = textAt("Standard");
        } else if (name == "$HANDSEED") {
            // Reserve every handle below the file's seed so new objects cannot
            // collide with ones the file will define later.
            if (const auto seed = Handle::tryParse(textAt("1"))) {
                db_->observeHandle(*seed);
            }
        } else {
            // Everything else is kept verbatim rather than discarded.
            std::vector<UnknownGroup> preserved;
            preserved.reserve(groups.size());
            for (const DxfGroup& group : groups) {
                preserved.push_back({group.code, group.asString()});
            }
            settings.unknownHeaderVariables[name] = std::move(preserved);
        }
    }

    // ------------------------------------------------------------- tables

    void readTables(GroupCursor& cursor) {
        // Which table's records are arriving. A table this reader models is
        // read record by record; one it does not is collected whole, because a
        // TABLES section written without it is a TABLES section that has lost
        // it. The VPORT table is the case in the sample drawing: it is how the
        // model space view is configured, and it is not this reader's to throw
        // away merely because nothing here reads it yet.
        static const std::set<std::string> modelled = {"LAYER", "LTYPE", "STYLE", "DIMSTYLE",
                                                       "BLOCK_RECORD", "APPID"};
        std::optional<PreservedSection> capturing;

        const auto finishCapture = [&] {
            if (capturing && !capturing->groups.empty()) {
                db_->preservedSections.push_back(std::move(*capturing));
            }
            capturing.reset();
        };

        for (;;) {
            const auto record = readRecord(cursor, {"ENDSEC"});
            if (!record || record->type() == "ENDSEC") {
                finishCapture();
                return;
            }

            const std::string type = normalizeSymbolName(record->type());

            const auto capture = [&](const DxfRecord& source) {
                capturing->groups.push_back({0, source.type()});
                for (const DxfGroup& group : source.groups()) {
                    capturing->groups.push_back({group.code, toPreservedValue(group)});
                }
            };

            if (type == "TABLE") {
                finishCapture();
                const std::string tableName = normalizeSymbolName(record->text(2));
                // Kept for the modelled tables too. Their records name the
                // table as their owner, and re-issuing the header a fresh
                // handle on save leaves every record in it owned by something
                // that is not in the file.
                if (const auto tableHandle = Handle::tryParse(record->text(5))) {
                    Database::SymbolTableHeader header;
                    header.handle = *tableHandle;
                    // Its `102 {...}` groups too. The LAYER table owns an
                    // extension dictionary holding this drawing's layer states.
                    bool inGroup = false;
                    for (const DxfGroup& group : record->groups()) {
                        if (group.code == 102) {
                            const std::string text = group.asString();
                            if (!inGroup && !text.empty() && text.front() == '{') {
                                inGroup = true;
                            } else if (inGroup && text == "}") {
                                header.applicationGroups.push_back({102, text});
                                inGroup = false;
                                continue;
                            }
                        }
                        if (inGroup) {
                            header.applicationGroups.push_back(
                                {group.code, toPreservedValue(group)});
                        }
                    }
                    db_->symbolTableHeaders[tableName] = std::move(header);
                }
                if (!modelled.contains(tableName)) {
                    capturing = PreservedSection{tableName, /*isTable=*/true, {}};
                    // The TABLE header goes in too, keeping its own handle.
                    // Its records name that handle as their owner, and a
                    // re-issued one would leave every record in the table
                    // owned by something that is not there.
                    capture(*record);
                }
                continue;
            }
            if (type == "ENDTAB") {
                if (capturing) {
                    capture(*record);
                }
                finishCapture();
                continue;
            }
            if (capturing) {
                capture(*record);
                continue;
            }

            // A STYLE with no name is a shape-file entry, not a text style,
            // and the style index is keyed by name. Kept verbatim rather than
            // dropped: the drawing registered that shape file for a reason.
            if (type == "STYLE" && record->text(2).empty()) {
                std::vector<UnknownGroup> verbatim;
                verbatim.push_back({0, record->type()});
                for (const DxfGroup& group : record->groups()) {
                    verbatim.push_back({group.code, toPreservedValue(group)});
                }
                db_->symbolTableHeaders["STYLE"].unmodelledRecords.push_back(std::move(verbatim));
                continue;
            }

            if (type == "LAYER") {
                readLayerRecord(*record);
            } else if (type == "LTYPE") {
                readLinetypeRecord(*record);
            } else if (type == "STYLE") {
                readTextStyleRecord(*record);
            } else if (type == "DIMSTYLE") {
                readDimStyleRecord(*record);
            } else if (type == "BLOCK_RECORD") {
                readBlockRecordEntry(*record);
            } else if (type == "APPID") {
                readAppIdRecord(*record);
            }
        }
    }

    /// Keep the registration itself, not merely the fact that something used it.
    ///
    /// An APPID with no XData anywhere is still the drawing's to keep: other CAD software
    /// writes registrations for tools that have touched the file, and deleting
    /// them because this session saw no data for them edits somebody else's
    /// drawing on their behalf.
    void readAppIdRecord(const DxfRecord& record) {
        const std::string name = record.text(2);
        if (name.empty()) {
            return;
        }
        std::vector<std::string>& applications = db_->registeredApplications;
        if (std::find(applications.begin(), applications.end(), name) == applications.end()) {
            applications.push_back(name);
        }
    }

    void readLayerRecord(const DxfRecord& record) {
        const std::string name = record.text(2);
        if (name.empty()) {
            return;
        }

        const bool isNew = db_->findLayer(name) == nullptr;
        auto owned = isNew ? std::make_unique<Layer>() : nullptr;
        Layer& layer = isNew ? *owned : *db_->findLayer(name);
        layer.name = name;

        if (isNew) {
            if (const auto handle = Handle::tryParse(record.text(5))) {
                layer.handle = *handle;
                // Reserved at once: the ensureLinetype below allocates, and
                // would otherwise be handed this handle.
                db_->observeHandle(*handle);
            }
        }

        const auto aci = static_cast<int>(record.integer(62, 7));
        // A negative ACI on a layer means the layer is switched off; the
        // magnitude remains its colour, needed for when it is turned on again.
        layer.isOff = aci < 0;
        if (const auto trueColor = record.optionalNumber(420)) {
            layer.color = Color::fromTrueColor(static_cast<std::uint32_t>(*trueColor));
        } else {
            layer.color = Color::fromIndex(std::abs(aci));
        }

        const auto flags = record.integer(70, 0);
        layer.isFrozen = (flags & 1) != 0;
        layer.isFrozenInNewViewports = (flags & 2) != 0;
        layer.isLocked = (flags & 4) != 0;
        layer.isXrefDependent = (flags & 16) != 0;

        layer.linetypeHandle = db_->ensureLinetype(record.text(6, kLinetypeContinuous)).handle;
        layer.lineweight = static_cast<std::int16_t>(record.integer(370, -3));
        layer.isPlottable = record.has(290) ? record.boolean(290, true) : true;
        layer.description = record.text(300);

        if (const auto transparency = record.optionalInteger(440);
            transparency && *transparency != 0) {
            layer.transparency = static_cast<std::uint8_t>(255 - (*transparency & 0xFF));
        }

        preserveRemainder(layer, record, "LAYER");
        if (isNew) {
            db_->addLayer(std::move(owned));
        }
    }

    void readLinetypeRecord(const DxfRecord& record) {
        const std::string name = record.text(2);
        if (name.empty()) {
            return;
        }

        const bool isNew = db_->findLinetype(name) == nullptr;
        auto owned = isNew ? std::make_unique<Linetype>() : nullptr;
        Linetype& linetype = isNew ? *owned : *db_->findLinetype(name);
        linetype.name = name;

        if (isNew) {
            if (const auto handle = Handle::tryParse(record.text(5))) {
                linetype.handle = *handle;
                db_->observeHandle(*handle);
            }
        }

        linetype.description = record.text(3);

        // Code 49 repeats once per dash element.
        linetype.elements.clear();
        for (const std::string& raw : record.allStrings(49)) {
            LinetypeElement element;
            element.length = std::strtod(raw.c_str(), nullptr);
            linetype.elements.push_back(std::move(element));
        }
        record.markConsumed({72, 73, 40, 74, 75, 340, 44, 45, 46, 50, 9});

        preserveRemainder(linetype, record, "LTYPE");
        if (isNew) {
            db_->addLinetype(std::move(owned));
        }
    }

    void readTextStyleRecord(const DxfRecord& record) {
        const std::string name = record.text(2);
        if (name.empty()) {
            return;
        }

        const bool isNew = db_->findTextStyle(name) == nullptr;
        auto owned = isNew ? std::make_unique<TextStyle>() : nullptr;
        TextStyle& style = isNew ? *owned : *db_->findTextStyle(name);
        style.name = name;

        if (isNew) {
            if (const auto handle = Handle::tryParse(record.text(5))) {
                style.handle = *handle;
                db_->observeHandle(*handle);
            }
        }

        style.height = record.number(40, 0.0);
        style.widthFactor = record.number(41, 1.0);
        style.obliqueAngle = degToRad(record.number(50, 0.0));
        style.fontFile = record.text(3, "txt.shx");
        style.bigFontFile = record.text(4);

        const auto generationFlags = record.integer(71, 0);
        style.isBackwards = (generationFlags & 2) != 0;
        style.isUpsideDown = (generationFlags & 4) != 0;

        const auto flags = record.integer(70, 0);
        style.isShapeFile = (flags & 1) != 0;
        style.isVertical = (flags & 4) != 0;

        preserveRemainder(style, record, "STYLE");
        if (isNew) {
            db_->addTextStyle(std::move(owned));
        }
    }

    /// DIMSTYLE, the one symbol table that does not use group 5 for its handle.
    ///
    /// It uses 105 instead, a quirk left over from DXF's history. Reading 5
    /// here would silently give every dimension style a fresh handle, breaking
    /// every dimension's reference to it.
    void readDimStyleRecord(const DxfRecord& record) {
        const std::string name = record.text(2);
        if (name.empty()) {
            return;
        }

        const bool isNew = db_->findDimStyle(name) == nullptr;
        auto owned = isNew ? std::make_unique<DimStyle>() : nullptr;
        DimStyle& style = isNew ? *owned : *db_->findDimStyle(name);
        style.name = name;

        if (isNew) {
            if (const auto handle = Handle::tryParse(record.text(105))) {
                style.handle = *handle;
                db_->observeHandle(*handle);
            }
        }

        style.overallScale = record.number(40, 1.0);

        style.arrowSize = record.number(41, 2.5);
        style.extensionLineOffset = record.number(42, 0.625);
        style.baselineSpacing = record.number(43, 3.75);
        style.extensionLineExtend = record.number(44, 1.25);
        style.roundOff = record.number(45, 0.0);
        style.dimensionLineExtend = record.number(46, 0.0);

        style.textHeight = record.number(140, 2.5);
        style.centerMarkSize = record.number(141, 2.5);
        style.linearScale = record.number(144, 1.0);
        style.textGap = record.number(147, 0.625);

        style.suppressExtensionLine1 = record.integer(75, 0) != 0;
        style.suppressExtensionLine2 = record.integer(76, 0) != 0;
        style.textInsideHorizontal = record.integer(73, 0) != 0;
        style.textOutsideHorizontal = record.integer(74, 0) != 0;

        if (const auto above = record.optionalInteger(77); above && *above >= 0 && *above <= 4) {
            style.textVerticalPosition = static_cast<TextVerticalPosition>(*above);
        }
        if (const auto justify = record.optionalInteger(280);
            justify && *justify >= 0 && *justify <= 4) {
            style.textHorizontalPosition = static_cast<TextHorizontalPosition>(*justify);
        }

        style.linearZeroSuppression =
            ZeroSuppression::fromDimzin(static_cast<int>(record.integer(78, 0)));
        style.angularZeroSuppression =
            ZeroSuppression::fromDimzin(static_cast<int>(record.integer(79, 0)));

        style.separateArrowBlocks = record.integer(173, 0) != 0;
        style.dimensionLineColor = Color::fromIndex(static_cast<int>(record.integer(176, 0)));
        style.extensionLineColor = Color::fromIndex(static_cast<int>(record.integer(177, 0)));
        style.textColor = Color::fromIndex(static_cast<int>(record.integer(178, 0)));
        style.angularPrecision = static_cast<int>(record.integer(179, 0));

        style.linearPrecision = static_cast<int>(record.integer(271, 2));
        if (const auto units = record.optionalInteger(277); units && *units >= 1 && *units <= 6) {
            style.linearFormat = static_cast<LinearUnitsFormat>(*units);
        }
        if (const auto angular = record.optionalInteger(275);
            angular && *angular >= 0 && *angular <= 4) {
            style.angularFormat = static_cast<AngularUnitsFormat>(*angular);
        }
        // DIMDSEP is the separator's *character code*, not the character.
        if (const auto separator = record.optionalInteger(278);
            separator && *separator > 0 && *separator < 128) {
            style.decimalSeparator = static_cast<char>(*separator);
        }

        style.suppressDimensionLine1 = record.integer(281, 0) != 0;
        style.suppressDimensionLine2 = record.integer(282, 0) != 0;

        style.linearPostfix = record.text(3);
        style.dimensionLineWeight =
            static_cast<std::int16_t>(record.integer(371, toRaw(Lineweight::ByBlock)));
        style.extensionLineWeight =
            static_cast<std::int16_t>(record.integer(372, toRaw(Lineweight::ByBlock)));

        style.fixedExtensionLines = record.integer(290, 0) != 0;
        style.fixedExtensionLength = record.number(49, 1.0);

        if (const auto textStyle = Handle::tryParse(record.text(340))) {
            style.textStyleHandle = *textStyle;
        } else {
            style.textStyleHandle = db_->ensureTextStyle(kTextStyleStandard).handle;
        }
        if (const auto block = Handle::tryParse(record.text(342))) { style.arrowBlock = *block; }
        if (const auto block = Handle::tryParse(record.text(343))) { style.arrowBlock1 = *block; }
        if (const auto block = Handle::tryParse(record.text(344))) { style.arrowBlock2 = *block; }

        preserveRemainder(style, record, "DIMSTYLE");
        if (isNew) {
            db_->addDimStyle(std::move(owned));
        }
    }

    /// BLOCK_RECORD: the table entry that gives a block its identity.
    ///
    /// Read here rather than left to the BLOCKS section, for two reasons. It
    /// carries the handle everything else references — layouts, entity owners,
    /// dimension style arrowheads — and it is the *only* place the paper-space
    /// records appear in some writers' output, so skipping it leaves every
    /// sheet pointing at a block that does not exist.
    ///
    /// The table precedes the BLOCKS section, so a record created here is
    /// found by name and filled in when its BLOCK arrives.
    void readBlockRecordEntry(const DxfRecord& record) {
        const std::string name = record.text(2);
        if (name.empty()) {
            return;
        }

        const bool isNew = db_->findBlock(name) == nullptr;
        auto owned = isNew ? std::make_unique<BlockTableRecord>() : nullptr;
        BlockTableRecord& block = isNew ? *owned : *db_->findBlock(name);
        block.name = name;

        if (isNew) {
            if (const auto handle = Handle::tryParse(record.text(5))) {
                block.handle = *handle;
                db_->observeHandle(*handle);
            }
        }

        if (const auto layout = Handle::tryParse(record.text(340))) {
            block.layoutHandle = *layout;
        }
        block.units = static_cast<int>(record.integer(70, 0));
        if (record.has(280)) {
            block.allowsExploding = record.integer(280, 1) != 0;
        }
        if (record.has(281)) {
            block.allowsNonUniformScaling = record.integer(281, 1) != 0;
        }

        preserveRemainder(block, record, "BLOCK_RECORD");
        if (isNew) {
            db_->addBlock(std::move(owned));
        }
    }

    // ------------------------------------------------------------ objects

    /// Read the OBJECTS section: layouts interpreted, everything else kept.
    ///
    /// This section holds dictionaries, plot settings, layer states, table and
    /// multileader styles, and a great deal of third-party data. Skipping it on
    /// read means *deleting* all of it on save. For a professional drawing that
    /// is the single most destructive thing a reader can do, and it does it
    /// silently — the file opens, looks right, and loses half its
    /// configuration the first time it is saved.
    void readObjects(GroupCursor& cursor) {
        for (;;) {
            const auto record = readRecord(cursor, {"ENDSEC"});
            if (!record || record->type() == "ENDSEC") {
                return;
            }
            if (normalizeSymbolName(record->type()) == "LAYOUT") {
                readLayoutRecord(*record);
                continue;
            }
            if (normalizeSymbolName(record->type()) == "FIELD") {
                readFieldRecord(*record);
                continue;
            }
            if (normalizeSymbolName(record->type()) == "IMAGEDEF") {
                readImageDefinitionRecord(*record);
                continue;
            }
            if (const auto kind = underlayDefinitionKind(record->type())) {
                readUnderlayDefinitionRecord(*record, *kind);
                continue;
            }
            readRawObject(*record);
        }
    }

    void readRawObject(const DxfRecord& record) {
        auto object = std::make_unique<RawObject>();
        object->dxfType = record.type();
        if (const auto handle = Handle::tryParse(record.text(5))) {
            object->handle = *handle;
            db_->observeHandle(*handle);
        }
        if (const auto owner = record.ownerHandleText()) {
            if (const auto ownerHandle = Handle::tryParse(*owner)) {
                object->ownerHandle = *ownerHandle;
            }
        }
        // Everything else, verbatim. Nothing here is interpreted, so nothing
        // can be lost by misinterpreting it.
        preserveRemainder(*object, record, record.type(), /*keepSubclassMarkers=*/true);
        db_->addObject(std::move(object));
    }

    /// Which underlay definition a record type names, if any.
    [[nodiscard]] static std::optional<UnderlayKind> underlayDefinitionKind(
        const std::string& type) {
        const std::string normalized = normalizeSymbolName(type);
        if (normalized == "PDFDEFINITION") {
            return UnderlayKind::Pdf;
        }
        if (normalized == "DWFDEFINITION") {
            return UnderlayKind::Dwf;
        }
        if (normalized == "DGNDEFINITION") {
            return UnderlayKind::Dgn;
        }
        return std::nullopt;
    }

    /// Read an IMAGEDEF: the file a raster image comes from.
    ///
    /// Scanned rather than consumed, on the same terms as a FIELD. Nothing here
    /// is edited yet — the interpreted values are a *view* of the groups, and a
    /// definition that comes back out byte for byte cannot lose the resolution
    /// units, the reactor list, or the third-party data that a partial rewrite
    /// would drop.
    void readImageDefinitionRecord(const DxfRecord& record) {
        auto definition = std::make_unique<ImageDefinition>();
        readObjectIdentity(*definition, record);

        bool haveFileName = false;
        for (const DxfGroup& group : record.groups()) {
            switch (group.code) {
                case 1:
                    if (!haveFileName) {
                        definition->fileName = group.asString();
                        haveFileName = true;
                    }
                    break;
                case 10: definition->sizeInPixels.x = group.asDouble(); break;
                case 20: definition->sizeInPixels.y = group.asDouble(); break;
                case 11: definition->pixelSize.x = group.asDouble(); break;
                case 21: definition->pixelSize.y = group.asDouble(); break;
                case 280: definition->loaded = group.asDouble() != 0.0; break;
                case 281: definition->resolutionUnits = static_cast<int>(group.asDouble()); break;
                default: break;
            }
        }

        preserveRemainder(*definition, record, record.type(), /*keepSubclassMarkers=*/true);
        db_->addObject(std::move(definition));
    }

    /// Read a PDFDEFINITION, DWFDEFINITION or DGNDEFINITION.
    void readUnderlayDefinitionRecord(const DxfRecord& record, UnderlayKind kind) {
        auto definition = std::make_unique<UnderlayDefinition>();
        definition->kind = kind;
        readObjectIdentity(*definition, record);

        bool haveFileName = false;
        bool haveItemName = false;
        for (const DxfGroup& group : record.groups()) {
            if (group.code == 1 && !haveFileName) {
                definition->fileName = group.asString();
                haveFileName = true;
            } else if (group.code == 2 && !haveItemName) {
                definition->itemName = group.asString();
                haveItemName = true;
            }
        }

        preserveRemainder(*definition, record, record.type(), /*keepSubclassMarkers=*/true);
        db_->addObject(std::move(definition));
    }

    /// Handle and owner, which every object in the OBJECTS section carries.
    void readObjectIdentity(DbObject& object, const DxfRecord& record) {
        if (const auto handle = Handle::tryParse(record.text(5))) {
            object.handle = *handle;
            db_->observeHandle(*handle);
        }
        if (const auto owner = record.ownerHandleText()) {
            if (const auto ownerHandle = Handle::tryParse(*owner)) {
                object.ownerHandle = *ownerHandle;
            }
        }
    }

    /// Read a FIELD: text whose value is derived rather than typed.
    ///
    /// Only the parts that decide what the field *says* are interpreted — the
    /// evaluator, the code, the children, the objects it names, and the cached
    /// result. The rest of the record is a nested value structure describing how
    /// the authoring application memoized it, and is kept verbatim: it is large,
    /// it is not needed to evaluate anything, and rewriting it from a partial
    /// understanding would be worse than not touching it.
    void readFieldRecord(const DxfRecord& record) {
        auto field = std::make_unique<FieldObject>();

        if (const auto handle = Handle::tryParse(record.text(5))) {
            field->handle = *handle;
            db_->observeHandle(*handle);
        }
        if (const auto owner = record.ownerHandleText()) {
            if (const auto ownerHandle = Handle::tryParse(*owner)) {
                field->ownerHandle = *ownerHandle;
            }
        }

        // Read by looking, not by claiming.
        //
        // A FIELD reuses its codes inside nested value blocks — code 1 is the
        // evaluator here and a property name three groups later — so consuming
        // a code would take every other occurrence with it and the record could
        // not be written back. Scanning the groups leaves all of them
        // unconsumed, so the whole record still round-trips verbatim while the
        // parts that decide what the field says are available as data.
        bool haveEvaluator = false;
        bool haveCode = false;
        for (const DxfGroup& group : record.groups()) {
            switch (group.code) {
                case 1:
                    if (!haveEvaluator) {
                        field->evaluatorId = group.asString();
                        haveEvaluator = true;
                    }
                    break;
                case 2:
                    if (!haveCode) {
                        field->fieldCode = group.asString();
                        haveCode = true;
                    }
                    break;
                case 360:
                    if (const auto handle = Handle::tryParse(group.asString())) {
                        field->childFields.push_back(*handle);
                    }
                    break;
                case 331:
                    if (const auto handle = Handle::tryParse(group.asString())) {
                        field->referencedObjects.push_back(*handle);
                    }
                    break;
                case 301:
                    // The last one wins: each nested value block carries one and
                    // the field's own result comes after them.
                    field->cachedValue = group.asString();
                    break;
                case 300:
                    // The format the value block was written with. Last one
                    // wins for the same reason.
                    field->appliedFormat = group.asString();
                    break;
                default: break;
            }
        }

        preserveRemainder(*field, record, "FIELD", /*keepSubclassMarkers=*/true);
        db_->addObject(std::move(field));
    }

    void readLayoutRecord(const DxfRecord& record) {
        auto layout = std::make_unique<Layout>();

        if (const auto handle = Handle::tryParse(record.text(5))) {
            layout->handle = *handle;
            db_->observeHandle(*handle);
        }

        // Code 330 appears several times in a LAYOUT and the occurrences mean
        // different things: the first is the owner — the ACAD_LAYOUT dictionary
        // — and one inside the AcDbLayout subclass is the *block record*
        // holding this sheet's geometry. Reading the first as the block points
        // every layout at the dictionary and leaves every sheet empty.
        //
        // They are told apart by what they resolve to rather than by position.
        // The positional rule looks simpler and breaks on the files that omit
        // the owner, and ODA writes the owner twice in a row, so counting is
        // not reliable either. Block records are read before this section, so
        // the lookup always has an answer.
        const std::vector<std::string> references = record.allStrings(330);
        for (std::size_t i = 0; i < references.size(); ++i) {
            const auto handle = Handle::tryParse(references[i]);
            if (!handle) {
                continue;
            }
            const auto* block = db_->get<BlockTableRecord>(*handle);
            if (block != nullptr && block->isLayoutSpace()) {
                layout->blockRecordHandle = *handle;
            } else if (i == 0) {
                layout->ownerHandle = *handle;
            }
        }

        // Code 1 appears twice in a LAYOUT: first as the plot settings' page
        // setup name, then as the layout's own name. The last occurrence is the
        // one that names the tab — reading the first gives every sheet the name
        // of its printer configuration, which is usually empty.
        const std::vector<std::string> names = record.allStrings(1);
        layout->name = names.empty() ? std::string{} : names.back();
        if (layout->name.empty()) {
            layout->name = "Layout";
        }

        layout->tabOrder = static_cast<int>(record.integer(71, 0));
        layout->minExtents = record.point(14);
        layout->maxExtents = record.point(15);
        // The limits, which for a layout are where the printable area lands in
        // paper-space coordinates. Codes 10 and 11 are free to mean this here
        // because AcDbPlotSettings, above in the same record, numbers its own
        // points from 40 upwards.
        layout->limitsMin = record.point(10);
        layout->limitsMax = record.point(11);

        PlotSettings& plot = layout->plot;
        // Code 2 is the printer configuration and code 4 the paper size name.
        // They are easy to swap and the swap is invisible until someone plots.
        plot.plotterName = record.text(2);
        plot.paperName = record.text(4);
        plot.marginLeft = record.number(40, 0.0);
        plot.marginBottom = record.number(41, 0.0);
        plot.marginRight = record.number(42, 0.0);
        plot.marginTop = record.number(43, 0.0);
        plot.paperWidth = record.number(44, 210.0);
        plot.paperHeight = record.number(45, 297.0);
        // Code 72 is the unit paper-space geometry is drawn in. Codes 40-45
        // above are millimetres whatever it says, so without this the sheet and
        // the title block sitting on it are measured in different units.
        switch (record.integer(72, static_cast<int>(PlotPaperUnits::Millimetres))) {
            case 0: plot.paperUnits = PlotPaperUnits::Inches; break;
            case 2: plot.paperUnits = PlotPaperUnits::Pixels; break;
            default: plot.paperUnits = PlotPaperUnits::Millimetres; break;
        }
        plot.numerator = record.number(142, 1.0);
        plot.denominator = record.number(143, 1.0);
        plot.rotation = static_cast<int>(record.integer(73, 0)) * 90;
        plot.plotUpsideDown = (record.integer(70, 0) & 0x4000) != 0;

        // 331 is the viewport that was last active when the drawing was saved,
        // not a list of viewports -- the sheet's viewports are found by looking
        // in its block record, which is where they actually live. It is left
        // unclaimed rather than consumed, so it survives to the file again:
        // nothing here reads it, and it is how other CAD software restores which viewport
        // the sheet was last worked in.
        record.markConsumed({333, 345, 346});

        Layout* raw = layout.get();
        preserveRemainder(*layout, record, "LAYOUT");
        db_->addObject(std::move(layout));

        // The block record may not have been read yet, and the link runs both
        // ways: the layout names its block, and the block names its layout.
        const Handle layoutHandle = raw->handle;
        deferred_.emplace_back([this, raw, layoutHandle] {
            if (raw->blockRecordHandle.isNull()) {
                // Older files leave 331 out and rely on the block record's own
                // back-reference instead.
                for (const Handle handle : db_->blocks().handles()) {
                    auto* block = db_->get<BlockTableRecord>(handle);
                    if (block != nullptr && block->layoutHandle == layoutHandle) {
                        raw->blockRecordHandle = handle;
                        return;
                    }
                }
                // Last resort: match Model to model space by name, which is the
                // one layout whose block is fixed by convention.
                if (raw->isModel()) {
                    raw->blockRecordHandle = db_->modelSpace().handle;
                }
                return;
            }
            if (auto* block = db_->get<BlockTableRecord>(raw->blockRecordHandle)) {
                block->layoutHandle = layoutHandle;
            }
        });
    }

    // ------------------------------------------------------------- blocks

    void readBlocks(GroupCursor& cursor) {
        for (;;) {
            const auto record = readRecord(cursor, {"ENDSEC"});
            if (!record || record->type() == "ENDSEC") {
                return;
            }
            if (normalizeSymbolName(record->type()) != "BLOCK") {
                continue;
            }

            const std::string name = record->text(2);
            if (name.empty()) {
                skipToBlockEnd(cursor);
                continue;
            }

            const bool isNew = db_->findBlock(name) == nullptr;
            auto owned = isNew ? std::make_unique<BlockTableRecord>() : nullptr;
            BlockTableRecord& block = isNew ? *owned : *db_->findBlock(name);
            block.name = name;

            if (isNew) {
                // The BLOCK entity's own handle (code 5) is *not* the handle
                // the rest of the file uses to refer to this block. That is the
                // BLOCK_RECORD's handle, which appears here as the BLOCK's
                // owner (code 330) — and it is what layouts, dimension style
                // arrowheads, and every contained entity's owner link name.
                //
                // Keying the record by code 5 leaves all of those dangling. It
                // looks correct on hand-written files, where the two are often
                // the same, and fails on every drawing other CAD software actually wrote.
                const auto ownerText = record->ownerHandleText();
                const auto recordHandle =
                    ownerText ? Handle::tryParse(*ownerText) : std::nullopt;
                const auto entityHandle = Handle::tryParse(record->text(5));
                if (const auto handle = recordHandle ? recordHandle : entityHandle) {
                    block.handle = *handle;
                    db_->observeHandle(*handle);
                }
                // Reserve the BLOCK entity's handle too, so a later allocation
                // cannot be handed a handle the file already uses.
                if (entityHandle) {
                    db_->observeHandle(*entityHandle);
                }
            }

            block.origin = record->point(10);
            block.description = record->text(4);

            const auto flags = record->integer(70, 0);
            // Bit 4 marks an external reference; bit 8 marks an overlay.
            if ((flags & 4) != 0) {
                block.xrefType = (flags & 8) != 0 ? XrefType::Overlay : XrefType::Attachment;
                block.xrefPath = record->text(1);
                // Resolution is the xref loader's job. Until it runs the
                // reference is simply unloaded, not missing — claiming
                // "not found" here would be an unearned assertion.
                block.xrefStatus = XrefStatus::Unloaded;
            }
            block.isXrefDependent = (flags & 16) != 0;

            BlockTableRecord* target = &block;
            if (isNew) {
                target = &db_->addBlock(std::move(owned));
            }
            blockNamesToHandles_[normalizeSymbolName(name)] = target->handle;

            readBlockEntities(cursor, *target);
        }
    }

    void readBlockEntities(GroupCursor& cursor, BlockTableRecord& block) {
        for (;;) {
            auto record = readRecord(cursor, {"ENDBLK", "ENDSEC"});
            if (!record) {
                return;
            }
            const std::string type = normalizeSymbolName(record->type());
            if (type == "ENDBLK") {
                return;
            }
            if (type == "ENDSEC") {
                // Malformed: a block was left open. Push the marker back so the
                // section loop terminates properly rather than consuming the
                // rest of the file.
                cursor.pushBack(DxfGroup{0, std::string{"ENDSEC"}, 0});
                return;
            }
            addEntityFromRecord(*record, block, cursor);
        }
    }

    void skipToBlockEnd(GroupCursor& cursor) {
        for (;;) {
            const auto group = cursor.next();
            if (!group) {
                return;
            }
            if (group->code == 0 && normalizeSymbolName(group->asString()) == "ENDBLK") {
                return;
            }
        }
    }

    // ----------------------------------------------------------- entities

    void readEntities(GroupCursor& cursor) {
        BlockTableRecord& modelSpace = ensureModelSpace();
        for (;;) {
            const auto record = readRecord(cursor, {"ENDSEC"});
            if (!record || record->type() == "ENDSEC") {
                return;
            }
            addEntityFromRecord(*record, ownerSpaceFor(*record, modelSpace), cursor);
        }
    }

    /// Which layout space an entity in the ENTITIES section belongs to.
    ///
    /// The section is not model space — it holds the entities of *every* layout,
    /// with code 330 naming the owning block record and code 67 flagging paper
    /// space for older files that omit the owner. Filing everything under model
    /// space puts every sheet's title block on top of the model, and then
    /// writes them all back out as model geometry.
    [[nodiscard]] BlockTableRecord& ownerSpaceFor(const DxfRecord& record,
                                                  BlockTableRecord& modelSpace) {
        // Read first, whichever way the owner is decided: a code left unread
        // is an unknown group, and an unknown group is written back verbatim
        // -- after the writer has already said 67 itself.
        const bool paperFlag = record.integer(67, 0) != 0;
        if (const auto owner = record.ownerHandleText()) {
            if (const auto ownerHandle = Handle::tryParse(*owner)) {
                if (auto* block = db_->get<BlockTableRecord>(*ownerHandle);
                    block != nullptr && block->isLayoutSpace()) {
                    return *block;
                }
            }
        }
        // No usable owner. Code 67 still distinguishes the two spaces, and for
        // a file with several sheets the first paper space is the best
        // available guess — better than silently merging them into the model.
        if (paperFlag) {
            if (auto* paper = db_->findBlock(kPaperSpace)) {
                return *paper;
            }
        }
        return modelSpace;
    }

    /// Convert a record to an entity and file it under `owner`.
    ///
    /// Handles the two DXF constructs where one logical entity spans several
    /// records: legacy POLYLINE with trailing VERTEX records, and INSERT with
    /// trailing ATTRIB records. Both terminate with SEQEND.
    void addEntityFromRecord(const DxfRecord& record, BlockTableRecord& owner,
                             GroupCursor& cursor) {
        const std::string type = normalizeSymbolName(record.type());
        // Structural markers produce no entity.
        if (type == "SEQEND" || type == "ENDBLK" || type == "BLOCK") {
            return;
        }

        std::unique_ptr<Entity> entity = readEntity(record);
        if (entity == nullptr) {
            return;
        }

        std::vector<Handle> attributeHandles;

        if (type == "POLYLINE") {
            if (auto* poly = dynamic_cast<PolylineEntity*>(entity.get())) {
                attachPolylineVertices(*poly, collectUntilSeqEnd(cursor, "VERTEX"));
            }
        } else if (type == "INSERT") {
            // Attributes follow the insert and belong to it. They are added as
            // entities in their own right so they can be selected and edited.
            for (const DxfRecord& attributeRecord : collectUntilSeqEnd(cursor, "ATTRIB")) {
                std::unique_ptr<Entity> attribute = readEntity(attributeRecord);
                if (attribute == nullptr) {
                    continue;
                }
                attributeHandles.push_back(db_->addEntity(std::move(attribute), owner).handle);
                ++entityCount_;
            }
        }

        Entity& added = db_->addEntity(std::move(entity), owner);
        ++entityCount_;

        if (auto* insert = dynamic_cast<InsertEntity*>(&added)) {
            insert->attributeHandles = std::move(attributeHandles);
        }
    }

    /// Read one entity record, falling back to a preserving placeholder for
    /// types with no registered reader.
    [[nodiscard]] std::unique_ptr<Entity> readEntity(const DxfRecord& record) {
        std::unique_ptr<Entity> entity;

        const EntityReader* reader = registry_.find(record.type());
        if (reader == nullptr) {
            auto placeholder = std::make_unique<UnsupportedEntity>();
            placeholder->dxfType = record.type();
            entity = std::move(placeholder);
        } else {
            try {
                entity = (*reader)(record, context_);
            } catch (const std::exception& error) {
                // One malformed entity must not abort a whole drawing.
                diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                        "Failed to read " + record.type() + ": " + error.what(),
                                        0});
                auto placeholder = std::make_unique<UnsupportedEntity>();
                placeholder->dxfType = record.type();
                entity = std::move(placeholder);
            }
        }

        if (entity == nullptr) {
            return nullptr;
        }
        readCommonProperties(*entity, record, context_);

        // An entity with a writer re-derives its own subclass markers, so its
        // preserved copies would be duplicates. One without a writer is echoed
        // back from the preserved groups, and *needs* them: a SOLID that came
        // back with no `100 AcDbTrace` has told the next reader that its four
        // corner points belong to no subclass at all.
        // A table is read for its meaning and written back for its bytes, on
        // the same terms as a FIELD: its per-cell blocks reuse group codes, so
        // nothing in it is claimed and there is no writer for it. That puts it
        // in the same position as an entity with no reader at all.
        // A multileader joins them for the same reason: its CONTEXT_DATA block
        // repeats group codes at three depths and overloads code 304, so
        // nothing in it is claimed and there is no writer for it.
        const bool verbatim = dynamic_cast<const UnsupportedEntity*>(entity.get()) != nullptr ||
                              dynamic_cast<const TableEntity*>(entity.get()) != nullptr ||
                              dynamic_cast<const MLeaderEntity*>(entity.get()) != nullptr;
        preserveRemainder(*entity, record, record.type(),
                          /*keepSubclassMarkers=*/verbatim);
        if (verbatim && entity->preserved != nullptr) {
            // Except `AcDbEntity`, which the common-property writer emits.
            auto& codes = entity->preserved->unknownCodes;
            const auto first = std::find_if(codes.begin(), codes.end(), [](const UnknownGroup& g) {
                const auto* text = std::get_if<std::string>(&g.value);
                return g.code == 100 && text != nullptr && *text == "AcDbEntity";
            });
            if (first != codes.end()) {
                codes.erase(first);
            }
        }
        return entity;
    }

    /// Collect records of `expectedType` until SEQEND.
    ///
    /// Anything else encountered terminates the run, so a file that omits its
    /// SEQEND — which happens — does not swallow the rest of the section.
    [[nodiscard]] std::vector<DxfRecord> collectUntilSeqEnd(GroupCursor& cursor,
                                                            std::string_view expectedType) {
        std::vector<DxfRecord> collected;
        for (;;) {
            const DxfGroup* peeked = cursor.peek();
            if (peeked == nullptr) {
                return collected;
            }
            if (peeked->code != 0) {
                static_cast<void>(cursor.next());
                continue;
            }

            const std::string type = normalizeSymbolName(peeked->asString());
            if (type == "SEQEND") {
                // Consume the SEQEND and its trailing groups; nothing in it is
                // needed, its presence is the whole signal.
                static_cast<void>(readRecord(cursor, {}));
                return collected;
            }
            if (type != normalizeSymbolName(expectedType)) {
                return collected;
            }

            auto record = readRecord(cursor, {});
            if (!record) {
                return collected;
            }
            collected.push_back(std::move(*record));
        }
    }

    /// Attach VERTEX records that followed a legacy POLYLINE.
    static void attachPolylineVertices(PolylineEntity& entity,
                                       const std::vector<DxfRecord>& vertexRecords) {
        entity.vertices.clear();
        for (const DxfRecord& record : vertexRecords) {
            const auto flags = record.integer(70, 0);
            // Flag 16 marks a spline control point; it is not a real polyline
            // corner and including it distorts the shape.
            if ((flags & 16) != 0) {
                continue;
            }
            entity.vertices.emplace_back(record.point(10), record.number(42, 0.0),
                                         record.number(40, entity.constantWidth),
                                         record.number(41, entity.constantWidth));
        }
    }

    // ------------------------------------------------------------- shared

    /// @param keepSubclassMarkers True for objects written back verbatim, which
    ///   have no writer of their own to re-derive their `100` markers. False for
    ///   typed objects, whose writers emit the right ones and would otherwise
    ///   emit each twice.
    static void preserveRemainder(DbObject& object, const DxfRecord& record,
                                  std::string originalType,
                                  bool keepSubclassMarkers = false) {
        object.extensionDictionaryHandle = extensionDictionaryOf(record);

        // Extended data belongs to every kind of record, not only to entities.
        //
        // It used to be copied in `readCommonProperties`, which entities alone
        // go through — so a layer's XData, a block record's, a layout's were
        // all read and then dropped on save. That is third-party application
        // data: a layer-state manager's settings, a cost-estimating add-in's
        // codes, whatever the drawing's own office attached. Copying it here
        // covers every record, because every reader ends up here.
        for (const auto& [application, items] : record.xdata()) {
            object.setXData(application, items);
        }

        std::vector<UnknownGroup> unknown = record.unconsumed();
        if (!keepSubclassMarkers) {
            std::erase_if(unknown,
                          [](const UnknownGroup& group) { return group.code == 100; });
        }
        // The `102 {...}` groups are lifted out and kept apart, because they
        // have to go back into the record's header rather than after its data.
        std::vector<UnknownGroup> application;
        std::vector<UnknownGroup> rest;
        for (std::size_t i = 0; i < unknown.size(); ++i) {
            const UnknownGroup& group = unknown[i];
            const auto* text = std::get_if<std::string>(&group.value);
            const bool opens = group.code == 102 && text != nullptr && !text->empty() &&
                               text->front() == '{';
            if (!opens) {
                rest.push_back(group);
                continue;
            }
            // Everything through the matching `102 }`, which is what makes the
            // group a group.
            for (; i < unknown.size(); ++i) {
                application.push_back(unknown[i]);
                const auto* closing = std::get_if<std::string>(&unknown[i].value);
                if (i != 0 && unknown[i].code == 102 && closing != nullptr && *closing == "}") {
                    break;
                }
            }
        }
        unknown = std::move(rest);

        if (unknown.empty() && application.empty() && record.embeddedGroups().empty()) {
            return;
        }
        object.preserved = std::make_unique<PreservedData>();
        object.preserved->applicationGroups = std::move(application);
        object.preserved->unknownCodes = std::move(unknown);
        object.preserved->embeddedObject = record.embeddedGroups();
        object.preserved->originalType = std::move(originalType);
    }

    /// The extension dictionary a record names, without consuming anything.
    ///
    /// Read by looking rather than claiming, deliberately: the `102
    /// {ACAD_XDICTIONARY}` group is written back from the preserved groups, and
    /// claiming the 360 inside it would take it out of them. What this produces
    /// is a resolved copy for the reader's own use, not a second source of
    /// truth.
    [[nodiscard]] static Handle extensionDictionaryOf(const DxfRecord& record) {
        bool inDictionaryGroup = false;
        for (const DxfGroup& group : record.groups()) {
            if (group.code == 102) {
                const std::string value = group.asString();
                inDictionaryGroup = value.starts_with("{ACAD_XDICTIONARY");
                continue;
            }
            if (group.code == 360 && inDictionaryGroup) {
                return Handle::tryParse(group.asString()).value_or(Handle{});
            }
        }
        return Handle{};
    }

    /// Resolve which field, if any, drives each object's text.
    ///
    /// The file states it as a chain: the object names an extension dictionary,
    /// that names a dictionary under `ACAD_FIELD`, and that names the field. The
    /// chain is walked once here so that everything afterwards — rendering,
    /// selection, the properties palette — can ask an object whether it is a
    /// field for the price of a handle comparison.
    void linkFieldsToTheirObjects() {
        const auto link = [this](DbObject& object) {
            if (object.extensionDictionaryHandle.isNull()) {
                return;
            }
            const DbObject* xdictionary = db_->getObject(object.extensionDictionaryHandle);
            if (xdictionary == nullptr) {
                return;
            }
            const Handle fieldDictionary = dictionaryEntry(*xdictionary, "ACAD_FIELD");
            const DbObject* dictionary =
                fieldDictionary.isNull() ? nullptr : db_->getObject(fieldDictionary);
            if (dictionary == nullptr) {
                return;
            }
            // The key names which of the object's properties the field drives —
            // `TEXT` for an MTEXT's contents. Only one is possible today, so the
            // first field found is the one, and reading the key would be
            // inventing a distinction nothing yet acts on.
            for (const auto& [name, handle] : dictionaryEntries(*dictionary)) {
                if (db_->get<FieldObject>(handle) != nullptr) {
                    object.fieldHandle = handle;
                    return;
                }
            }
        };

        for (const Handle handle : db_->objectHandles()) {
            if (DbObject* object = db_->getObject(handle)) {
                link(*object);
            }
        }
        for (const Handle blockHandle : db_->blocks().handles()) {
            if (auto* block = db_->get<BlockTableRecord>(blockHandle)) {
                for (Entity* entity : db_->entitiesIn(*block)) {
                    link(*entity);
                }
            }
        }
    }

    /// Read groups up to the next code-0 marker, returning them as a record.
    ///
    /// The terminating marker is left unconsumed unless it is in `stopTypes`,
    /// in which case an empty record of that type is returned so the caller can
    /// react.
    [[nodiscard]] std::optional<DxfRecord> readRecord(
        GroupCursor& cursor, std::initializer_list<std::string_view> stopTypes) {
        // Find the code-0 that names this record.
        auto typeGroup = cursor.next();
        while (typeGroup && typeGroup->code != 0) {
            typeGroup = cursor.next();
        }
        if (!typeGroup) {
            return std::nullopt;
        }

        const std::string type = typeGroup->asString();
        const std::string normalized = normalizeSymbolName(type);
        for (const std::string_view stop : stopTypes) {
            if (normalized == normalizeSymbolName(stop)) {
                return DxfRecord{normalized, {}};
            }
        }

        std::vector<DxfGroup> groups;
        for (;;) {
            const DxfGroup* peeked = cursor.peek();
            if (peeked == nullptr || peeked->code == 0) {
                break;
            }
            groups.push_back(*cursor.next());
        }
        return DxfRecord{type, std::move(groups)};
    }

    const EntityReaderRegistry& registry_;
    std::unique_ptr<Database> db_;
    std::vector<DxfDiagnostic> diagnostics_;
    std::vector<std::function<void()>> deferred_;
    std::unordered_map<std::string, Handle> blockNamesToHandles_;
    /// Declared after the members it references, so member initialization order
    /// guarantees those are alive when it is constructed.
    ReadContext context_;
    std::string version_;
    std::size_t entityCount_ = 0;
};

DxfReader::DxfReader() : registry_(EntityReaderRegistry::standard()) {}

DxfReader::DxfReader(EntityReaderRegistry registry) : registry_(std::move(registry)) {}

DxfReadResult DxfReader::read(std::string_view text) {
    Impl impl{registry_};
    return impl.read(text);
}

DxfReadResult readDxf(std::string_view text) {
    DxfReader reader;
    return reader.read(text);
}

}  // namespace cad::io
