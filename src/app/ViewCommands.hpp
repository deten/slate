#pragma once

#include <functional>
#include <string>
#include <utility>
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
/// Attach a drawing file as an external reference: make its block record and
/// load its contents. Returns the problem, or an empty string and the block's
/// handle. Given by the window because loading needs the file layer.
using AttachXrefHook =
    std::function<std::string(const std::string& path, bool overlay, Handle* block)>;

/// What the attach commands ask of the file layer. Each returns the problem
/// in words, or an empty string and the answer through its pointers.
struct AttachHooks {
    AttachXrefHook xref;
    /// The project's sheets, number and title in order, for the index
    /// sheet. Empty when no project is open.
    std::function<std::vector<std::pair<std::string, std::string>>()> sheetList;
    /// Write a drawing to a file, DXF or DWG by its extension. Returns the
    /// problem, or an empty string. Given by the window because DWG goes
    /// through the translator it holds.
    std::function<std::string(const Database& db, const std::string& path)> writeDrawing;
    /// How many pages a PDF has.
    std::function<std::string(const std::string& path, int* pages)> pdfPages;
    /// A page's size in points, pages counted from one.
    std::function<std::string(const std::string& path, int page, double* width, double* height)>
        pdfPage;
    /// An image's size in pixels.
    std::function<std::string(const std::string& path, int* width, int* height)> imageSize;
};

[[nodiscard]] std::vector<CommandDefinition> viewCommands(
    CanvasWidget* canvas, std::function<bool(const std::string&)> switchLayout = {},
    std::function<void()> quickSelect = {},
    std::function<void()> references = {}, std::function<void()> purge = {},
    std::function<void(const std::string&)> assistant = {}, AttachHooks hooks = {});

}  // namespace cad::app
