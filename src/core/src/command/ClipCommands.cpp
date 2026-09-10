// Clipping what is brought in from outside: a block reference or an
// external reference by XCLIP, a picture by IMAGECLIP.
//
// A background shows the floor a sheet is about and not the whole
// building; a photograph shows the corner it was taken for. Both keep the
// whole and show a part, so the part can be changed or the whole shown
// again. The boundary is kept the way the file keeps it, so the clip means
// the same everywhere the drawing goes.
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/ClipFilter.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"

namespace cad {
namespace {

[[nodiscard]] bool matchesKeyword(std::string_view text, char letter) {
    if (text.empty()) {
        return false;
    }
    return static_cast<char>(std::toupper(static_cast<unsigned char>(text.front()))) == letter;
}

/// The stages shared by both clips: pick the thing, choose what to do,
/// take the boundary as two corners or as points until Enter.
class ClipCommandBase : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (context.initialSelection().size() == 1) {
            return takeTarget(context.initialSelection().front(), context);
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = selectPrompt();
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::Target:
                if (input.kind != InputValue::Kind::Entity) {
                    return StepResult::cancelled();
                }
                return takeTarget(input.entity, context);
            case Stage::Option: {
                if (input.kind == InputValue::Kind::Empty) {
                    stage_ = Stage::Shape;
                    return askShape();
                }
                if (input.kind != InputValue::Kind::Keyword && input.kind != InputValue::Kind::Text) {
                    return StepResult::cancelled();
                }
                if (matchesKeyword(input.text, 'N')) {
                    stage_ = Stage::Shape;
                    return askShape();
                }
                if (matchesKeyword(input.text, 'D')) {
                    return remove(context);
                }
                if (matchesKeyword(input.text, 'O')) {
                    // ON or OFF: the second letter tells them apart.
                    const bool on = input.text.size() < 3 ||
                                    std::toupper(static_cast<unsigned char>(input.text[1])) == 'N';
                    return enable(on, context);
                }
                return StepResult::failed("Unknown option.");
            }
            case Stage::Shape: {
                if (input.kind == InputValue::Kind::Empty) {
                    polygonal_ = false;
                    stage_ = Stage::Points;
                    return askCorner("Specify first corner");
                }
                if (input.kind != InputValue::Kind::Keyword && input.kind != InputValue::Kind::Text) {
                    return StepResult::cancelled();
                }
                polygonal_ = matchesKeyword(input.text, 'P');
                stage_ = Stage::Points;
                return polygonal_ ? askCorner("Specify first point") : askCorner("Specify first corner");
            }
            case Stage::Points:
            default:
                if (input.kind == InputValue::Kind::Point) {
                    points_.push_back(input.point);
                    if (!polygonal_ && points_.size() == 2) {
                        return apply(context);
                    }
                    return askCorner(polygonal_ ? "Specify next point, or Enter to finish"
                                                : "Specify opposite corner");
                }
                if (input.kind != InputValue::Kind::Empty || !polygonal_) {
                    return StepResult::cancelled();
                }
                if (points_.size() < 3) {
                    return StepResult::cancelled("A boundary needs at least three points.");
                }
                return apply(context);
        }
    }

protected:
    [[nodiscard]] virtual std::string selectPrompt() const = 0;
    [[nodiscard]] virtual std::string acceptTarget(Handle handle, CommandContext& context) = 0;
    virtual StepResult apply(CommandContext& context) = 0;
    virtual StepResult enable(bool on, CommandContext& context) = 0;
    virtual StepResult remove(CommandContext& context) = 0;

    /// The boundary as picked, in world coordinates: a rectangle from two
    /// corners, or the polygon's points.
    [[nodiscard]] std::vector<Vec3> worldBoundary() const {
        if (polygonal_) {
            return points_;
        }
        const Vec3 a = points_[0];
        const Vec3 b = points_[1];
        return {a, Vec3{b.x, a.y, a.z}, b, Vec3{a.x, b.y, a.z}};
    }

    Handle target_;

private:
    enum class Stage : std::uint8_t { Target, Option, Shape, Points };

    StepResult takeTarget(Handle handle, CommandContext& context) {
        const std::string problem = acceptTarget(handle, context);
        if (!problem.empty()) {
            return StepResult::failed(problem);
        }
        target_ = handle;
        stage_ = Stage::Option;
        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Enter option [ON/OFF/Delete/New boundary] <New>";
        request.keywords = {
            CommandKeyword{"ON", "ON", "Show only what is inside the boundary"},
            CommandKeyword{"OFF", "OFF", "Show the whole, keeping the boundary"},
            CommandKeyword{"Delete", "D", "Drop the boundary"},
            CommandKeyword{"New", "N", "Draw a new boundary"},
        };
        request.defaultText = "New";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askShape() {
        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Boundary [Rectangular/Polygonal] <Rectangular>";
        request.keywords = {
            CommandKeyword{"Rectangular", "R", "Two corners"},
            CommandKeyword{"Polygonal", "P", "Points until Enter"},
        };
        request.defaultText = "Rectangular";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult askCorner(std::string prompt) const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = std::move(prompt);
        if (!points_.empty()) {
            request.basePoint = points_.back();
        }
        request.allowEmpty = polygonal_ && points_.size() >= 3;
        return StepResult::ask(request);
    }

    Stage stage_ = Stage::Target;
    bool polygonal_ = false;
    std::vector<Vec3> points_;
};

/// XCLIP: clip a block reference or an external reference.
class XclipCommand final : public ClipCommandBase {
protected:
    [[nodiscard]] std::string selectPrompt() const override {
        return "Select the block or reference to clip";
    }

    [[nodiscard]] std::string acceptTarget(Handle handle, CommandContext& context) override {
        const auto* insert = context.db().get<InsertEntity>(handle);
        if (insert == nullptr || insert->erased) {
            return "That is not a block reference.";
        }
        return {};
    }

    StepResult apply(CommandContext& context) override {
        auto* insert = context.db().get<InsertEntity>(target_);
        if (insert == nullptr) {
            return StepResult::failed("The reference is gone.");
        }
        // The boundary is picked in the world and kept in the block's own
        // coordinates, so it moves with the reference.
        const std::optional<Mat4> toBlock = insert->blockTransform().inverted();
        if (!toBlock.has_value()) {
            return StepResult::failed("The reference has no size to clip within.");
        }
        std::vector<Vec3> boundary;
        for (const Vec3& p : worldBoundary()) {
            boundary.push_back(toBlock->transformPoint(p));
        }
        context.transaction().openForWrite(*insert);
        insert->clipBoundary = std::move(boundary);
        insert->clipEnabled = true;
        writeClipFilter(context.db(), *insert, context.transaction());
        return StepResult::done("Reference clipped to " + std::to_string(insert->clipBoundary.size()) +
                                " points");
    }

    StepResult enable(bool on, CommandContext& context) override {
        auto* insert = context.db().get<InsertEntity>(target_);
        if (insert == nullptr) {
            return StepResult::failed("The reference is gone.");
        }
        if (insert->clipBoundary.size() < 3) {
            return StepResult::failed("The reference has no clip boundary. Choose New.");
        }
        context.transaction().openForWrite(*insert);
        insert->clipEnabled = on;
        writeClipFilter(context.db(), *insert, context.transaction());
        return StepResult::done(on ? "Clip on" : "Clip off");
    }

    StepResult remove(CommandContext& context) override {
        auto* insert = context.db().get<InsertEntity>(target_);
        if (insert == nullptr) {
            return StepResult::failed("The reference is gone.");
        }
        removeClipFilter(context.db(), *insert, context.transaction());
        return StepResult::done("Clip removed");
    }
};

/// IMAGECLIP: clip a picture.
class ImageClipCommand final : public ClipCommandBase {
protected:
    [[nodiscard]] std::string selectPrompt() const override { return "Select the image to clip"; }

    [[nodiscard]] std::string acceptTarget(Handle handle, CommandContext& context) override {
        const auto* image = context.db().get<RasterImageEntity>(handle);
        if (image == nullptr || image->erased) {
            return "That is not an image.";
        }
        return {};
    }

    StepResult apply(CommandContext& context) override {
        auto* image = context.db().get<RasterImageEntity>(target_);
        if (image == nullptr) {
            return StepResult::failed("The image is gone.");
        }
        // The image's clip is kept in its own pixels: solve each world
        // point against the pixel vectors from the image's origin.
        const Vec3 u = image->uVector;
        const Vec3 v = image->vVector;
        const double det = u.x * v.y - u.y * v.x;
        if (std::abs(det) < 1e-12) {
            return StepResult::failed("The image has no size to clip within.");
        }
        std::vector<Vec3> pixels;
        for (const Vec3& p : worldBoundary()) {
            const Vec3 d = p - image->origin;
            const double px = (d.x * v.y - d.y * v.x) / det;
            const double py = (u.x * d.y - u.y * d.x) / det;
            pixels.emplace_back(px, py, 0.0);
        }
        context.transaction().openForWrite(*image);
        image->clipVertices = std::move(pixels);
        image->clipBoundaryType = image->clipVertices.size() == 4 ? 1 : 2;
        // A rectangle is kept as its two corners, as the file has it.
        if (image->clipBoundaryType == 1) {
            const Vec3 a = image->clipVertices[0];
            const Vec3 c = image->clipVertices[2];
            image->clipVertices = {a, c};
        }
        image->clippingEnabled = true;
        image->useClippingBoundary = true;
        return StepResult::done("Image clipped");
    }

    StepResult enable(bool on, CommandContext& context) override {
        auto* image = context.db().get<RasterImageEntity>(target_);
        if (image == nullptr) {
            return StepResult::failed("The image is gone.");
        }
        if (image->clipVertices.size() < 2) {
            return StepResult::failed("The image has no clip boundary. Choose New.");
        }
        context.transaction().openForWrite(*image);
        image->clippingEnabled = on;
        return StepResult::done(on ? "Clip on" : "Clip off");
    }

    StepResult remove(CommandContext& context) override {
        auto* image = context.db().get<RasterImageEntity>(target_);
        if (image == nullptr) {
            return StepResult::failed("The image is gone.");
        }
        context.transaction().openForWrite(*image);
        image->clipVertices.clear();
        image->clippingEnabled = false;
        image->clipBoundaryType = 1;
        return StepResult::done("Clip removed");
    }
};

}  // namespace

std::vector<CommandDefinition> clipCommands() {
    std::vector<CommandDefinition> commands;
    {
        CommandDefinition xclip;
        xclip.id = "cad.insert.xclip";
        xclip.name = "XCLIP";
        xclip.aliases = {"XC"};
        xclip.title = "Clip Reference";
        xclip.description = "Show only part of a block or external reference";
        xclip.category = "Insert";
        xclip.supportsPickfirst = true;
        xclip.create = [] { return std::make_unique<XclipCommand>(); };
        commands.push_back(std::move(xclip));
    }
    {
        CommandDefinition clip;
        clip.id = "cad.insert.imageclip";
        clip.name = "IMAGECLIP";
        clip.aliases = {"ICL"};
        clip.title = "Clip Image";
        clip.description = "Show only part of a picture";
        clip.category = "Insert";
        clip.supportsPickfirst = true;
        clip.create = [] { return std::make_unique<ImageClipCommand>(); };
        commands.push_back(std::move(clip));
    }
    return commands;
}

}  // namespace cad
