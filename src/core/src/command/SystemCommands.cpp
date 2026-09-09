// Building-system drafting: a duct run in one command.
//
// A run of duct is drawn the same way every time in an office: on the
// layer for its system, as a wide single line or as two lines the duct's
// width apart, with its size written along it. Drawing that from lines and
// polylines takes a dozen inputs and the conventions have to be remembered
// each time. DUCT takes the system, the size and the route, and draws the
// rest the office way -- which is also what makes it one line for an
// assistant to write and one line for a person to read.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "cad/agent/DrawingReading.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/geom/Operations.hpp"
#include "cad/geom/Polyline.hpp"
#include "cad/math/Angle.hpp"

namespace cad {
namespace {

[[nodiscard]] bool matchesKeyword(std::string_view text, char letter) {
    if (text.empty()) {
        return false;
    }
    const auto first = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    return first == letter;
}

[[nodiscard]] std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/// "24x12" -> 24 wide, 12 deep; "12" -> a round duct 12 across; "24X12",
/// "24 x 12", "24*12" all read. Zero when nothing readable.
struct DuctSize {
    double width = 0.0;
    double depth = 0.0;
    std::string text;
    [[nodiscard]] bool valid() const noexcept { return width > 0.0; }
};

[[nodiscard]] DuctSize parseSize(std::string_view raw) {
    DuctSize size;
    std::string cleaned;
    for (const char c : raw) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.') {
            cleaned.push_back(c);
        } else if (c == 'x' || c == 'X' || c == '*' || c == '/') {
            cleaned.push_back('x');
        }
    }
    const auto x = cleaned.find('x');
    try {
        if (x == std::string::npos) {
            size.width = cleaned.empty() ? 0.0 : std::stod(cleaned);
            size.depth = size.width;
            size.text = cleaned;
        } else {
            size.width = std::stod(cleaned.substr(0, x));
            size.depth = std::stod(cleaned.substr(x + 1));
            size.text = cleaned.substr(0, x) + "x" + cleaned.substr(x + 1);
        }
    } catch (const std::exception&) {
        size.width = 0.0;
    }
    if (!std::isfinite(size.width) || !std::isfinite(size.depth)) {
        size.width = 0.0;
    }
    return size;
}

/// The layer for a system: the drawing's own, when it has one named for
/// it, else the layer-standard name, made with the usual colour.
[[nodiscard]] Handle layerFor(std::string_view system, CommandContext& context) {
    Database& db = context.db();
    const std::string wanted = std::string(system) + " duct";
    for (const Handle handle : db.layers().handles()) {
        const auto* layer = db.get<Layer>(handle);
        if (layer != nullptr && !layer->erased && systemKindOf(layer->name) == wanted &&
            systemStatusOf(layer->name).empty()) {
            return handle;
        }
    }
    const char* name = system == "supply" ? "M-DUCT-SUPP"
                       : system == "return" ? "M-DUCT-RETN"
                                            : "M-DUCT-EXHS";
    const int colour = system == "supply" ? 4 : system == "return" ? 6 : 1;
    if (Layer* existing = db.findLayer(name); existing != nullptr) {
        return existing->handle;
    }
    auto layer = std::make_unique<Layer>();
    layer->name = name;
    layer->color = Color::fromIndex(colour);
    return context.transaction().addObject(std::move(layer)).handle;
}

/// DUCT: a run of duct along a route.
///
/// Inputs, in order: the system (Supply, Return or Exhaust, or Single or
/// Double to say how it is drawn), the size, then the route's points until
/// Enter. A run is a wide polyline, or two polylines the duct's width apart
/// when drawn double, with the size written along its longest leg.
class DuctCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override { return askSystem(); }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::System: {
                if (input.kind == InputValue::Kind::Empty) {
                    stage_ = Stage::Size;
                    return askSize();
                }
                if (input.kind != InputValue::Kind::Keyword && input.kind != InputValue::Kind::Text) {
                    return StepResult::cancelled();
                }
                const std::string word = input.text;
                if (matchesKeyword(word, 'D')) {
                    doubleLine_ = true;
                    return askSystem();
                }
                if (matchesKeyword(word, 'L')) {
                    doubleLine_ = false;
                    return askSystem();
                }
                if (matchesKeyword(word, 'R')) {
                    system_ = "return";
                } else if (matchesKeyword(word, 'E')) {
                    system_ = "exhaust";
                } else if (matchesKeyword(word, 'S')) {
                    system_ = "supply";
                } else {
                    // Not a system: the size was given first, which is a
                    // fair way to write it.
                    const DuctSize size = parseSize(word);
                    if (!size.valid()) {
                        return StepResult::failed("\"" + word + "\" is not a system or a size.");
                    }
                    size_ = size;
                    stage_ = Stage::Route;
                    return askPoint();
                }
                stage_ = Stage::Size;
                return askSize();
            }
            case Stage::Size: {
                std::string text;
                if (input.kind == InputValue::Kind::Text || input.kind == InputValue::Kind::Keyword) {
                    text = input.text;
                } else if (input.kind == InputValue::Kind::Number) {
                    text = std::to_string(input.number);
                } else {
                    return StepResult::cancelled();
                }
                const DuctSize size = parseSize(text);
                if (!size.valid()) {
                    return StepResult::failed("\"" + text + "\" is not a size like 24x12 or 12.");
                }
                size_ = size;
                stage_ = Stage::Route;
                return askPoint();
            }
            case Stage::Route:
            default: {
                if (input.kind == InputValue::Kind::Point) {
                    if (route_.empty() || !route_.back().equals(input.point, Tolerance{1e-9})) {
                        route_.push_back(input.point);
                    }
                    return askPoint();
                }
                if (input.kind != InputValue::Kind::Empty) {
                    return StepResult::cancelled();
                }
                if (route_.size() < 2) {
                    return StepResult::cancelled("A run needs at least two points.");
                }
                return draw(context);
            }
        }
    }

private:
    enum class Stage : std::uint8_t { System, Size, Route };

    [[nodiscard]] StepResult askSystem() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Enter system [Supply/Return/Exhaust] or how it is drawn [Line/Double]";
        request.keywords = {
            CommandKeyword{"Supply", "S", "Supply air"},
            CommandKeyword{"Return", "R", "Return air"},
            CommandKeyword{"Exhaust", "E", "Exhaust air"},
            CommandKeyword{"Line", "L", "A single wide line"},
            CommandKeyword{"Double", "D", "Two lines the duct's width apart"},
        };
        request.defaultText = "Supply";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askSize() {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter size (width x depth, or diameter)";
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult askPoint() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = route_.empty() ? "Specify start of run"
                                        : "Specify next point, or Enter to finish";
        if (!route_.empty()) {
            request.basePoint = route_.back();
        }
        request.allowEmpty = !route_.empty();
        return StepResult::ask(request);
    }

    StepResult draw(CommandContext& context) {
        const Handle layer = layerFor(system_, context);
        const double width = size_.width;
        int made = 0;

        if (doubleLine_) {
            // Two lines the duct's width apart, each side offset from the
            // centreline so corners meet as they would in a fitting.
            const Polyline centre = PolylineEntity::fromPoints(route_).geometry();
            for (const double side : {width / 2.0, -width / 2.0}) {
                CurvePtr offset = offsetCurve(centre, side);
                const auto* poly = dynamic_cast<const Polyline*>(offset.get());
                if (poly == nullptr) {
                    continue;
                }
                auto entity = std::make_unique<PolylineEntity>(PolylineEntity::fromPolyline(*poly));
                entity->layerHandle = layer;
                context.transaction().addToCurrentSpace(std::move(entity));
                ++made;
            }
        } else {
            auto entity = std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(route_));
            entity->constantWidth = width;
            entity->layerHandle = layer;
            context.transaction().addToCurrentSpace(std::move(entity));
            ++made;
        }

        // The size along the longest leg, just off the duct on its left,
        // reading along the run.
        std::size_t longest = 0;
        double longestLength = -1.0;
        for (std::size_t i = 0; i + 1 < route_.size(); ++i) {
            const double length = route_[i].distanceTo(route_[i + 1]);
            if (length > longestLength) {
                longestLength = length;
                longest = i;
            }
        }
        const Vec3 a = route_[longest];
        const Vec3 b = route_[longest + 1];
        const Vec3 direction = (b - a).normalized();
        double angle = std::atan2(direction.y, direction.x);
        Vec3 along = direction;
        // Text reads left to right, or bottom to top, never upside down.
        if (angle > kPi / 2.0 + 1e-9 || angle < -kPi / 2.0 + 1e-9) {
            angle += kPi;
            along = along * -1.0;
        }
        const Vec3 left{-along.y, along.x, 0.0};
        const double height = std::max(width * 0.35, 1e-6);
        const Vec3 mid = a.midpoint(b);
        const double textWidth = measureText(size_.text, height, 1.0);
        const Vec3 at = mid + left * (width / 2.0 + height * 0.5) - along * (textWidth / 2.0);
        auto tag = std::make_unique<TextEntity>(at, size_.text, height);
        tag->rotation = angle;
        tag->layerHandle = layer;
        context.transaction().addToCurrentSpace(std::move(tag));

        return StepResult::done(std::string(doubleLine_ ? "Double-line " : "") + system_ +
                                " duct " + size_.text + " drawn, " +
                                std::to_string(route_.size() - 1) + " leg" +
                                (route_.size() == 2 ? "" : "s"));
    }

    Stage stage_ = Stage::System;
    std::string system_ = "supply";
    bool doubleLine_ = false;
    DuctSize size_;
    std::vector<Vec3> route_;
};

}  // namespace

std::vector<CommandDefinition> systemCommands() {
    std::vector<CommandDefinition> commands;
    CommandDefinition duct;
    duct.id = "cad.system.duct";
    duct.name = "DUCT";
    duct.aliases = {"DU"};
    duct.title = "Duct";
    duct.description = "Draw a run of duct: system, size, route";
    duct.category = "Draw";
    duct.supportsPickfirst = false;
    duct.create = [] { return std::make_unique<DuctCommand>(); };
    commands.push_back(std::move(duct));
    return commands;
}

}  // namespace cad
