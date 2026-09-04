#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cad::io {

/// What kind of resource is being looked for, so the right paths are searched.
enum class ResourceKind : std::uint8_t {
    Drawing,
    Image,
    Font,
    Linetype,
    HatchPattern,
    PlotStyle,
};

/// Where a resource was eventually found.
enum class ResolutionSource : std::uint8_t {
    /// The stored path was absolute and existed.
    Absolute,
    /// Found relative to the host drawing.
    DrawingRelative,
    /// Found on a configured search path.
    SearchPath,
    /// Found by filename alone after the stored path failed.
    FilenameFallback,
    NotFound,
};

struct ResolutionResult {
    bool found = false;
    /// Absolute path, when found.
    std::filesystem::path path;
    ResolutionSource source = ResolutionSource::NotFound;
    /// The path as stored in the file, always preserved unchanged.
    std::string originalPath;
    /// Every location tried, for a diagnostic the user can act on.
    std::vector<std::filesystem::path> attempted;
};

struct ResolverOptions {
    /// Directory of the drawing doing the referencing.
    std::filesystem::path hostDirectory;
    /// Configured search paths, in priority order.
    std::vector<std::filesystem::path> searchPaths;
    /// Fall back to searching for the bare filename when the stored path fails.
    ///
    /// On by default because it is overwhelmingly what a user wants: a project
    /// copied from another machine has correct filenames and wrong directories.
    bool filenameFallback = true;
};

/// Locates the files a drawing depends on: xrefs, raster images, fonts,
/// linetype and hatch pattern definitions, plot style tables.
///
/// Resolution order deliberately mirrors other CAD software's, because users' folder
/// structures are built around it:
///
///  1. The stored path, if absolute and present.
///  2. Relative to the host drawing's own folder — the case that makes a
///     project folder portable, and the most common in practice.
///  3. Each configured search path in order.
///  4. The bare filename, as a last resort.
///
/// The stored path is never rewritten as a side effect of resolution. A drawing
/// authored with relative paths must stay relative when saved, or the project
/// stops being portable for everyone else.
class ResourceResolver {
public:
    explicit ResourceResolver(ResolverOptions options);

    [[nodiscard]] ResolutionResult resolve(const std::string& storedPath,
                                           ResourceKind kind = ResourceKind::Drawing);

    void addSearchPath(const std::filesystem::path& path);

    [[nodiscard]] const std::vector<std::filesystem::path>& searchPaths() const noexcept {
        return options_.searchPaths;
    }

    /// A resolver for a nested reference, rooted at the referencing drawing.
    ///
    /// Nested references inherit the host's search paths, and the host's own
    /// folder becomes one so siblings remain findable.
    [[nodiscard]] ResourceResolver forNestedDrawing(
        const std::filesystem::path& drawingPath) const;

    void clearCache() { cache_.clear(); }

private:
    [[nodiscard]] ResolutionResult resolveUncached(const std::string& storedPath,
                                                   ResourceKind kind) const;

    ResolverOptions options_;
    /// Loading a drawing with fifty xrefs, each naming the same font, would
    /// otherwise hit the filesystem repeatedly for identical questions.
    std::map<std::string, ResolutionResult> cache_;
};

/// Convert stored path separators to the host platform's.
///
/// DWG files routinely contain backslash paths regardless of authoring
/// platform, and a drawing produced on Windows must still resolve its xrefs
/// when opened on macOS.
[[nodiscard]] std::filesystem::path normalizeSeparators(const std::string& path);

}  // namespace cad::io
