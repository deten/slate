#pragma once

#include <memory>
#include <vector>

#include "cad/command/Command.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {

/// The drawing commands: LINE, PLINE, CIRCLE, ARC, RECTANG, POINT, POLYGON.
[[nodiscard]] std::vector<CommandDefinition> drawCommands();

/// The modify commands: MOVE, COPY, ROTATE, SCALE, MIRROR, ERASE, OFFSET,
/// TRIM, EXTEND, FILLET, plus UNDO and REDO.
[[nodiscard]] std::vector<CommandDefinition> modifyCommands();

/// The reshaping commands: CHAMFER, BREAK, JOIN, EXPLODE and STRETCH.
[[nodiscard]] std::vector<CommandDefinition> reshapeCommands();

/// Whether TEXTEDIT would find anything to edit on this entity.
///
/// Asked by the shell before a double-click starts the command, so clicking a
/// polyline does not open an annotation command that then asks the user to go
/// and find it some annotation.
[[nodiscard]] bool hasEditableText(const Entity& entity);

/// The text creation commands: TEXT and MTEXT.
[[nodiscard]] std::vector<CommandDefinition> textCommands();

/// UPDATEFIELD: recompute text whose value is derived from the drawing.
[[nodiscard]] std::vector<CommandDefinition> fieldCommands();

/// PURGE: remove named objects nothing refers to.
[[nodiscard]] std::vector<CommandDefinition> purgeCommands();

/// The block commands: BLOCK and INSERT.
[[nodiscard]] std::vector<CommandDefinition> blockCommands();

/// LAYER: make a layer and draw on it.
[[nodiscard]] std::vector<CommandDefinition> layerCommands();

/// The annotation commands: the DIM* family and HATCH.
[[nodiscard]] std::vector<CommandDefinition> annotationCommands();

/// MVIEW and PAGESETUP: viewports on a sheet, and the sheet's paper and scale.
[[nodiscard]] std::vector<CommandDefinition> layoutCommands();

/// ATTDEF and ATTEDIT: the tagged text a block asks for, and changing it.
[[nodiscard]] std::vector<CommandDefinition> attributeCommands();

/// Building-system drafting: DUCT.
[[nodiscard]] std::vector<CommandDefinition> systemCommands();

/// Drawing in the style of an example: LIKE.
[[nodiscard]] std::vector<CommandDefinition> likeCommands();

/// Clipping references and pictures: XCLIP, IMAGECLIP.
[[nodiscard]] std::vector<CommandDefinition> clipCommands();

/// Build a registry populated with the built-in commands.
///
/// Deliberately a function returning a fresh registry rather than a shared
/// singleton: each open document gets its own, so a plugin registering a
/// command for one drawing cannot leak into another, and tests never share
/// state.
///
/// The built-ins go through exactly the same `registerCommand` call a plugin
/// would use. If that path breaks, every command breaks — a far better failure
/// mode than plugin registration silently rotting because nothing exercises it.
[[nodiscard]] std::unique_ptr<CommandRegistry> createStandardRegistry();

/// Build a regular polygon.
///
/// A circumscribed polygon touches the circle at its edge midpoints, so its
/// vertices sit further out by `1 / cos(pi / n)` — the distinction users rely
/// on when drawing bolt heads to an across-flats dimension.
[[nodiscard]] Polyline regularPolygon(const Vec3& center, double radius, int sides,
                                      bool inscribed);

/// Wrap a bare curve in the entity type that represents it.
///
/// Editing operations return geometry; this converts it back to database
/// objects. Keeping the mapping in one place means every operation produces
/// consistent entity types.
[[nodiscard]] std::unique_ptr<Entity> entityFromCurve(const Curve& curve);

/// Copy presentation properties from one entity to another, so a replacement
/// produced by an edit is visually indistinguishable from the original.
void copyProperties(const Entity& source, Entity& target);

/// Clone an entity, keeping its concrete type and dropping its handle.
///
/// `DbObject::clone` returns the base pointer, so every caller otherwise
/// repeats the same downcast-and-release dance. The clone comes back with a
/// null handle, so adding it to a database allocates a fresh one rather than
/// colliding with the original.
[[nodiscard]] std::unique_ptr<Entity> cloneEntity(const Entity& entity);

/// The model-space entity nearest a pick point, ignoring `exclude`.
///
/// Shared rather than duplicated per command file: "what did the user click on"
/// must mean the same thing in every command, or FILLET and CHAMFER disagree
/// about which of two touching lines was picked.
[[nodiscard]] Entity* nearestEntityAt(const Vec3& point, CommandContext& context,
                                      const std::vector<Handle>& exclude = {});

}  // namespace cad
