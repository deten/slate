/// The two commands that make a sheet: MVIEW opens a window onto the model,
/// PAGESETUP says what paper the sheet prints on and at what scale.
///
/// Both act on the *current space* -- the sheet whose tab is showing. MVIEW
/// refuses to run in model space: a viewport there would be a window onto
/// itself.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/ViewportEntity.hpp"

namespace cad {
namespace {

const CommandKeyword kFitKeyword{"Fit", "F", "Fill the printable area"};

[[nodiscard]] bool matchesKeyword(const std::string& typed, char initial) {
    return !typed.empty() &&
           std::toupper(static_cast<unsigned char>(typed.front())) == initial;
}

/// A scale as people write it -- "1:100", "1/100", "1=100", or just "100"
/// for 1:100 -- as model units per paper unit. Nothing for text that is not
/// a scale.
[[nodiscard]] std::optional<double> parseScale(const std::string& text) {
    std::string compact;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            compact.push_back(c);
        }
    }
    if (compact.empty()) {
        return std::nullopt;
    }
    try {
        const std::size_t split = compact.find_first_of(":/=");
        if (split == std::string::npos) {
            const double n = std::stod(compact);
            return n > 0.0 ? std::optional<double>{n} : std::nullopt;
        }
        const double paper = std::stod(compact.substr(0, split));
        const double model = std::stod(compact.substr(split + 1));
        if (paper <= 0.0 || model <= 0.0) {
            return std::nullopt;
        }
        return model / paper;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// "1:100" for a whole ratio, "1:2.5" otherwise.
[[nodiscard]] std::string describeScale(double modelPerPaper) {
    if (std::abs(modelPerPaper - std::round(modelPerPaper)) < 1e-9) {
        return "1:" + std::to_string(static_cast<long long>(std::llround(modelPerPaper)));
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "1:%.3g", modelPerPaper);
    return buffer;
}

// ---------------------------------------------------------------- MVIEW

/// MVIEW: a viewport on the current sheet, by two corners or fitted to the
/// printable area, showing the model at a scale or zoomed to its extents.
class MviewCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        Database& db = context.db();
        if (db.currentSpace().isModelSpace()) {
            return StepResult::failed(
                "MVIEW makes a viewport on a sheet. Switch to a layout tab first.");
        }
        return askCorner();
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::FirstCorner:
                if (input.kind == InputValue::Kind::Keyword && matchesKeyword(input.text, 'F')) {
                    const Box3 area = printableRect(context);
                    if (area.isEmpty() || area.width() <= 0.0 || area.height() <= 0.0) {
                        return StepResult::failed("This sheet has no printable area to fit.");
                    }
                    first_ = area.min;
                    second_ = area.max;
                    stage_ = Stage::Scale;
                    return askScale();
                }
                if (input.kind != InputValue::Kind::Point) {
                    return askCorner();
                }
                first_ = input.point;
                stage_ = Stage::SecondCorner;
                return askOpposite();

            case Stage::SecondCorner:
                if (input.kind != InputValue::Kind::Point) {
                    return askOpposite();
                }
                second_ = input.point;
                if (std::abs(second_.x - first_.x) < 1e-9 || std::abs(second_.y - first_.y) < 1e-9) {
                    return StepResult::failed("The viewport has no area.");
                }
                stage_ = Stage::Scale;
                return askScale();

            case Stage::Scale:
                if (input.kind == InputValue::Kind::Keyword && matchesKeyword(input.text, 'F')) {
                    return create(std::nullopt, context);
                }
                if (input.kind == InputValue::Kind::Empty ||
                    (input.kind == InputValue::Kind::Text &&
                     (input.text.empty() || matchesKeyword(input.text, 'F')))) {
                    return create(std::nullopt, context);
                }
                if (input.kind == InputValue::Kind::Number) {
                    if (input.number <= 0.0) {
                        return StepResult::failed("A scale must be positive.");
                    }
                    return create(input.number, context);
                }
                if (input.kind == InputValue::Kind::Text) {
                    if (const auto scale = parseScale(input.text)) {
                        return create(*scale, context);
                    }
                    return StepResult::failed("\"" + input.text +
                                              "\" is not a scale. Write it as 1:100, or 100.");
                }
                return askScale();
        }
        return StepResult::cancelled();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& /*context*/) const override {
        if (stage_ != Stage::SecondCorner) {
            return {};
        }
        const Vec3 corners[] = {first_, Vec3{cursor.x, first_.y, 0.0}, cursor,
                                Vec3{first_.x, cursor.y, 0.0}};
        std::vector<std::unique_ptr<Entity>> items;
        items.push_back(std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(corners, true)));
        return items;
    }

private:
    enum class Stage { FirstCorner, SecondCorner, Scale };

    [[nodiscard]] static StepResult askCorner() {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify corner of viewport or [Fit]";
        request.keywords.push_back(kFitKeyword);
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult askOpposite() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify opposite corner";
        request.basePoint = first_;
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askScale() {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter viewport scale as paper:model (1:100) or [Fit] <Fit>";
        request.keywords.push_back(kFitKeyword);
        request.defaultText = "Fit";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] static Box3 printableRect(CommandContext& context) {
        Database& db = context.db();
        if (const Layout* layout = db.layoutForBlock(db.currentSpace().handle)) {
            return layout->printableRect();
        }
        return Box3::empty();
    }

    [[nodiscard]] static int nextViewportId(Database& db, const BlockTableRecord& space) {
        // 1 is the sheet's own paper view; windows onto the model start at 2.
        int highest = 1;
        for (const Entity* entity : db.entitiesIn(space)) {
            if (const auto* window = dynamic_cast<const ViewportEntity*>(entity)) {
                highest = std::max(highest, window->viewportId);
            }
        }
        return highest + 1;
    }

    StepResult create(std::optional<double> modelPerPaper, CommandContext& context) {
        Database& db = context.db();
        BlockTableRecord& space = db.currentSpace();
        const Box3 window = Box3::fromCorners(first_, second_);
        const double width = window.width();
        const double height = window.height();

        // What the window looks at: the whole model, centred. A scale fixes
        // how much of it fits; without one the extents fill the window, the
        // way a fresh other CAD software viewport zooms to extents.
        const Box3 model = db.blockExtents(db.modelSpace());
        auto window_entity = std::make_unique<ViewportEntity>();
        window_entity->center = window.center();
        window_entity->width = width;
        window_entity->height = height;
        window_entity->viewportId = nextViewportId(db, space);
        window_entity->statusFlags = 0;
        window_entity->viewCenter = model.isEmpty() ? Vec3{} : model.center();
        if (modelPerPaper) {
            window_entity->viewHeight = height * *modelPerPaper;
        } else if (model.isEmpty()) {
            window_entity->viewHeight = height;
        } else {
            // Fit both ways round: the extents may be wider than the window
            // is, in which case width decides how much height shows.
            window_entity->viewHeight =
                std::max(model.height(), model.width() * height / width);
        }
        const double scale = window_entity->viewHeight / height;
        const int id = window_entity->viewportId;
        context.transaction().addToCurrentSpace(std::move(window_entity));
        context.log("Viewport " + std::to_string(id) + " at " + describeScale(scale) +
                    (modelPerPaper ? "" : " (fitted to the model)"));
        return StepResult::done();
    }

    Stage stage_ = Stage::FirstCorner;
    Vec3 first_;
    Vec3 second_;
};

// ------------------------------------------------------------ PAGESETUP

struct PaperSize {
    const char* name;
    double width;   // millimetres, portrait
    double height;
};

constexpr PaperSize kPapers[] = {
    {"A4", 210.0, 297.0},      {"A3", 297.0, 420.0},      {"A2", 420.0, 594.0},
    {"A1", 594.0, 841.0},      {"A0", 841.0, 1189.0},     {"Letter", 215.9, 279.4},
    {"Legal", 215.9, 355.6},   {"Tabloid", 279.4, 431.8},
};

[[nodiscard]] std::string lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

/// The paper whose name the text begins, if exactly one does.
[[nodiscard]] const PaperSize* paperNamed(const std::string& text) {
    const std::string typed = lower(text);
    const PaperSize* found = nullptr;
    for (const PaperSize& paper : kPapers) {
        const std::string name = lower(paper.name);
        if (name == typed) {
            return &paper;
        }
        if (name.compare(0, typed.size(), typed) == 0) {
            if (found != nullptr) {
                return nullptr;  // ambiguous: "L" is Letter or Legal
            }
            found = &paper;
        }
    }
    return found;
}

/// The paper the settings describe, by size, or nothing for a custom sheet.
[[nodiscard]] const PaperSize* paperOf(const PlotSettings& plot) {
    for (const PaperSize& paper : kPapers) {
        if (std::abs(plot.paperWidth - paper.width) < 0.5 &&
            std::abs(plot.paperHeight - paper.height) < 0.5) {
            return &paper;
        }
    }
    return nullptr;
}

/// PAGESETUP: paper, orientation and plot scale for the current sheet, from
/// the command line -- the three settings a sheet cannot do without.
class PageSetupCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        Database& db = context.db();
        const Layout* layout = db.layoutForBlock(db.currentSpace().handle);
        if (layout == nullptr) {
            return StepResult::failed("The current space has no layout to set up.");
        }
        const PaperSize* current = paperOf(layout->plot);
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter paper size [A4/A3/A2/A1/A0/Letter/Legal/Tabloid]";
        request.defaultText = current != nullptr ? current->name : "A4";
        request.allowEmpty = true;
        for (const PaperSize& paper : kPapers) {
            request.keywords.push_back(CommandKeyword{paper.name, paper.name, ""});
        }
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        Database& db = context.db();
        Layout* layout = db.layoutForBlock(db.currentSpace().handle);
        if (layout == nullptr) {
            return StepResult::failed("The current space has no layout to set up.");
        }
        switch (stage_) {
            case Stage::Paper: {
                const std::string typed = textOf(input);
                if (typed.empty()) {
                    paper_ = paperOf(layout->plot);
                    if (paper_ == nullptr) {
                        paper_ = &kPapers[0];
                    }
                } else {
                    paper_ = paperNamed(typed);
                    if (paper_ == nullptr) {
                        return StepResult::failed("\"" + typed + "\" is not a paper size here.");
                    }
                }
                stage_ = Stage::Orientation;
                InputRequest request;
                request.kind = InputRequest::Kind::Text;
                request.prompt = "Enter orientation [Portrait/Landscape]";
                request.defaultText = layout->plot.isRotatedQuarterTurn() ? "Landscape" : "Portrait";
                request.allowEmpty = true;
                request.keywords = {CommandKeyword{"Portrait", "P", ""},
                                    CommandKeyword{"Landscape", "L", ""}};
                return StepResult::ask(request);
            }
            case Stage::Orientation: {
                const std::string typed = textOf(input);
                if (typed.empty()) {
                    landscape_ = layout->plot.isRotatedQuarterTurn();
                } else if (matchesKeyword(typed, 'L')) {
                    landscape_ = true;
                } else if (matchesKeyword(typed, 'P')) {
                    landscape_ = false;
                } else {
                    return StepResult::failed("Portrait or Landscape.");
                }
                stage_ = Stage::Scale;
                InputRequest request;
                request.kind = InputRequest::Kind::Text;
                request.prompt = "Enter plot scale as paper:model";
                const double denominator =
                    layout->plot.denominator > 0.0 ? layout->plot.denominator : 1.0;
                const double numerator = layout->plot.numerator > 0.0 ? layout->plot.numerator : 1.0;
                request.defaultText = describeScale(denominator / numerator);
                request.allowEmpty = true;
                return StepResult::ask(request);
            }
            case Stage::Scale: {
                const std::string typed = textOf(input);
                std::optional<double> modelPerPaper;
                if (typed.empty()) {
                    const double denominator =
                        layout->plot.denominator > 0.0 ? layout->plot.denominator : 1.0;
                    const double numerator =
                        layout->plot.numerator > 0.0 ? layout->plot.numerator : 1.0;
                    modelPerPaper = denominator / numerator;
                } else if (input.kind == InputValue::Kind::Number && input.number > 0.0) {
                    modelPerPaper = input.number;
                } else {
                    modelPerPaper = parseScale(typed);
                }
                if (!modelPerPaper) {
                    return StepResult::failed("\"" + typed +
                                              "\" is not a scale. Write it as 1:100, or 100.");
                }
                return apply(*modelPerPaper, *layout, context);
            }
        }
        return StepResult::cancelled();
    }

private:
    enum class Stage { Paper, Orientation, Scale };

    [[nodiscard]] static std::string textOf(const InputValue& input) {
        if (input.kind == InputValue::Kind::Text || input.kind == InputValue::Kind::Keyword) {
            return input.text;
        }
        if (input.kind == InputValue::Kind::Number) {
            return std::to_string(input.number);
        }
        return {};
    }

    StepResult apply(double modelPerPaper, Layout& target, CommandContext& context) {
        Layout& layout = context.transaction().openForWrite(target);
        PlotSettings& plot = layout.plot;
        // Stored the way other CAD software stores a page: the portrait paper, turned by
        // the rotation, in millimetres.
        plot.paperName = std::string{paper_->name};
        plot.paperWidth = paper_->width;
        plot.paperHeight = paper_->height;
        plot.paperUnits = PlotPaperUnits::Millimetres;
        plot.rotation = landscape_ ? 90 : 0;
        if (plot.marginLeft == 0.0 && plot.marginRight == 0.0 && plot.marginBottom == 0.0 &&
            plot.marginTop == 0.0) {
            plot.marginLeft = plot.marginRight = plot.marginBottom = plot.marginTop = 10.0;
        }
        plot.numerator = 1.0;
        plot.denominator = modelPerPaper;
        // The stored limits say where an old page sat; a new page sits at the
        // origin, so they are cleared rather than left pointing at the old one.
        layout.limitsMin = Vec3{};
        layout.limitsMax = Vec3{};

        context.log(std::string{paper_->name} + (landscape_ ? " landscape" : " portrait") +
                    " at " + describeScale(modelPerPaper) + " for " + layout.name);
        return StepResult::done();
    }

    Stage stage_ = Stage::Paper;
    const PaperSize* paper_ = nullptr;
    bool landscape_ = false;
};

[[nodiscard]] CommandDefinition define(std::string id, std::string name,
                                       std::vector<std::string> aliases, std::string title,
                                       std::string description,
                                       std::function<CommandInstancePtr()> create) {
    CommandDefinition definition;
    definition.id = std::move(id);
    definition.name = std::move(name);
    definition.aliases = std::move(aliases);
    definition.title = std::move(title);
    definition.description = std::move(description);
    definition.category = "View";
    definition.create = std::move(create);
    return definition;
}

}  // namespace

std::vector<CommandDefinition> layoutCommands() {
    std::vector<CommandDefinition> commands;
    commands.push_back(define("cad.layout.mview", "MVIEW", {"MV"}, "Viewport",
                              "Open a window onto the model on this sheet",
                              [] { return std::make_unique<MviewCommand>(); }));
    commands.push_back(define("cad.layout.pagesetup", "PAGESETUP", {}, "Page Setup",
                              "Paper, orientation and scale for this sheet",
                              [] { return std::make_unique<PageSetupCommand>(); }));
    return commands;
}

}  // namespace cad
