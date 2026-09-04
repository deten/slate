#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cad/command/CommandRegistry.hpp"

namespace cad::app {

class CanvasWidget;

/// ZOOM and PAN, which act on the *view* rather than the drawing.
///
/// These live in the shell rather than the core, and are registered through the
/// same `registerCommand` a plugin uses. That is not a workaround: the core
/// deliberately has no idea what a viewport is — `CommandContext` offers a
/// database and a transaction and nothing about a camera — and putting ZOOM in
/// it would mean giving the engine a notion of a view purely so one command
/// could reach it.
///
/// It also exercises the extension point with something real, which is the best
/// evidence that a plugin could add a command of its own.
///
/// @param canvas the view these commands drive. Must outlive the registry they
///   are registered in, which the window guarantees by owning both.
/// @param switchLayout activates a layout by name, returning false when there
///   is no such tab. A callback rather than a Document pointer because
///   switching sheets also has to refresh the tab bar and the view, which is
///   the window's business rather than the document's.
[[nodiscard]] std::vector<CommandDefinition> viewCommands(
    CanvasWidget* canvas, std::function<bool(const std::string&)> switchLayout = {},
    std::function<void()> quickSelect = {},
    std::function<void()> references = {}, std::function<void()> purge = {},
    std::function<void(const std::string&)> assistant = {});

}  // namespace cad::app
