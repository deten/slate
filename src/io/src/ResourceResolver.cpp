#include "cad/io/ResourceResolver.hpp"

#include <algorithm>
#include <system_error>

namespace cad::io {
namespace {

[[nodiscard]] bool isReadableFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    // The non-throwing overloads matter: probing many candidate paths, most of
    // which do not exist, must not be an exception-driven loop.
    return std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] std::string trim(const std::string& text) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end && isSpace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (end != begin && isSpace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string{begin, end};
}

/// Default locations searched for a given resource kind.
///
/// Empty for now: shipping a guess at where a user's fonts live is worse than
/// searching nothing, because a wrong hit is harder to diagnose than a miss.
/// These come from settings instead.
[[nodiscard]] std::vector<std::filesystem::path> defaultPathsFor(ResourceKind /*kind*/) {
    return {};
}

}  // namespace

std::filesystem::path normalizeSeparators(const std::string& path) {
    std::string converted = path;
    const char preferred = static_cast<char>(std::filesystem::path::preferred_separator);
    for (char& c : converted) {
        if (c == '\\' || c == '/') {
            c = preferred;
        }
    }
    return std::filesystem::path{converted}.lexically_normal();
}

ResourceResolver::ResourceResolver(ResolverOptions options) : options_(std::move(options)) {}

void ResourceResolver::addSearchPath(const std::filesystem::path& path) {
    if (std::find(options_.searchPaths.begin(), options_.searchPaths.end(), path) ==
        options_.searchPaths.end()) {
        options_.searchPaths.push_back(path);
        // Previously failed lookups might now succeed.
        cache_.clear();
    }
}

ResolutionResult ResourceResolver::resolve(const std::string& storedPath, ResourceKind kind) {
    const std::string key = std::to_string(static_cast<int>(kind)) + ":" + storedPath;
    if (const auto it = cache_.find(key); it != cache_.end()) {
        return it->second;
    }
    ResolutionResult result = resolveUncached(storedPath, kind);
    cache_.emplace(key, result);
    return result;
}

ResolutionResult ResourceResolver::resolveUncached(const std::string& storedPath,
                                                   ResourceKind kind) const {
    ResolutionResult result;
    result.originalPath = storedPath;

    const std::string trimmed = trim(storedPath);
    if (trimmed.empty()) {
        return result;
    }

    // DWG stores Windows-style paths even in files authored elsewhere, so
    // separators are normalized before anything touches the filesystem.
    const std::filesystem::path normalized = normalizeSeparators(trimmed);

    const auto succeed = [&](const std::filesystem::path& path, ResolutionSource source) {
        result.found = true;
        result.path = path;
        result.source = source;
        return result;
    };

    // 1. An absolute stored path.
    if (normalized.is_absolute()) {
        result.attempted.push_back(normalized);
        if (isReadableFile(normalized)) {
            return succeed(normalized, ResolutionSource::Absolute);
        }
    }

    // 2. Relative to the host drawing.
    const std::filesystem::path hostRelative =
        (options_.hostDirectory / normalized).lexically_normal();
    result.attempted.push_back(hostRelative);
    if (isReadableFile(hostRelative)) {
        return succeed(hostRelative, ResolutionSource::DrawingRelative);
    }

    // 3. Configured search paths, plus the defaults for this resource kind.
    std::vector<std::filesystem::path> roots = options_.searchPaths;
    const std::vector<std::filesystem::path> defaults = defaultPathsFor(kind);
    roots.insert(roots.end(), defaults.begin(), defaults.end());

    for (const std::filesystem::path& root : roots) {
        const std::filesystem::path candidate = (root / normalized).lexically_normal();
        result.attempted.push_back(candidate);
        if (isReadableFile(candidate)) {
            return succeed(candidate, ResolutionSource::SearchPath);
        }
    }

    // 4. The bare filename, which rescues projects moved between machines.
    if (options_.filenameFallback) {
        const std::filesystem::path name = normalized.filename();
        if (name != normalized) {
            std::vector<std::filesystem::path> fallbackRoots{options_.hostDirectory};
            fallbackRoots.insert(fallbackRoots.end(), roots.begin(), roots.end());

            for (const std::filesystem::path& root : fallbackRoots) {
                const std::filesystem::path candidate = (root / name).lexically_normal();
                if (std::find(result.attempted.begin(), result.attempted.end(), candidate) !=
                    result.attempted.end()) {
                    continue;
                }
                result.attempted.push_back(candidate);
                if (isReadableFile(candidate)) {
                    return succeed(candidate, ResolutionSource::FilenameFallback);
                }
            }
        }
    }

    return result;
}

ResourceResolver ResourceResolver::forNestedDrawing(
    const std::filesystem::path& drawingPath) const {
    ResolverOptions nested;
    nested.hostDirectory = drawingPath.parent_path();
    nested.searchPaths.push_back(options_.hostDirectory);
    nested.searchPaths.insert(nested.searchPaths.end(), options_.searchPaths.begin(),
                              options_.searchPaths.end());
    nested.filenameFallback = options_.filenameFallback;
    return ResourceResolver{std::move(nested)};
}

}  // namespace cad::io
