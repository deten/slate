#pragma once

#include <functional>
#include <vector>

#include "cad/command/CommandRegistry.hpp"

namespace cad {
class BlockTableRecord;
}

namespace cad::app {

/// The name Slate puts its geometry on the system clipboard under.
///
/// A private format rather than plain text: a DXF fragment pasted into a text
/// editor is noise, and a text editor's contents pasted into a drawing are not
/// geometry. Anything that cannot read this type sees an empty clipboard from
/// us, which is the honest answer.
inline constexpr const char* kClipboardMimeType = "application/x-slate-dxf";

/// COPYCLIP, CUTCLIP, PASTECLIP and PASTEORIG.
///
/// These live in the shell rather than the core for the same reason ZOOM does:
/// the system clipboard is an operating-system service, and `cad_core` links
/// nothing but the standard library. The part that is genuinely about drawings
/// — working out what a selection depends on and merging it into another
/// database — is in `cad::extractEntities` and `cad::pasteContent`, where it is
/// testable without a windowing system and reusable by anything that needs to
/// copy objects between drawings.
///
/// The payload is a DXF fragment. It costs a little to write and parse, and in
/// exchange the clipboard carries layers, linetypes, text styles and block
/// definitions with the geometry, survives between two separately running
/// copies of the application, and is inspectable when something goes wrong.
///
/// @param activeSpace where a paste lands: the layout or block currently being
///   edited. Null, or a callback returning null, pastes into model space.
[[nodiscard]] std::vector<CommandDefinition> clipboardCommands(
    std::function<BlockTableRecord*()> activeSpace = {});

}  // namespace cad::app
