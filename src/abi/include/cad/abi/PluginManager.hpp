#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cad/abi/PluginHost.hpp"

namespace cad::abi {

/// What a loaded plugin contributed, for the plugin manager UI and diagnostics.
struct PluginInfo {
    std::filesystem::path path;
    /// File stem, used as the plugin's display name.
    std::string name;
    std::uint32_t versionMajor = 0;
    std::uint32_t versionMinor = 0;
    std::vector<std::string> commandIds;
};

/// The outcome of a load attempt.
///
/// A value rather than an exception: a plugin failing to load is an ordinary
/// event that the application reports and carries on from. One bad DLL in the
/// plugins folder must not stop the others loading, and must certainly not stop
/// the application starting.
struct PluginLoadResult {
    bool loaded = false;
    std::filesystem::path path;
    std::string message;
};

/// Loads and unloads plugin shared libraries.
///
/// The only part of the system that touches the OS dynamic loader, which is why
/// it lives here rather than in the core: `cad_core` must stay free of platform
/// APIs, and loading a DLL is unambiguously one.
class PluginManager {
public:
    PluginManager(CommandRegistry& registry, HostDocument& document);
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

    /// Load one plugin.
    ///
    /// The version is checked before any of the plugin's code runs, which is
    /// the only point at which an incompatible plugin can still be refused
    /// safely.
    PluginLoadResult load(const std::filesystem::path& path);

    /// Load every shared library in a directory, non-recursively.
    ///
    /// Results are returned in path order rather than filesystem order, so a
    /// drawing that depends on two plugins behaves the same on every machine.
    std::vector<PluginLoadResult> loadDirectory(const std::filesystem::path& directory);

    /// Unload one plugin, removing its commands first.
    bool unload(const std::filesystem::path& path);

    void unloadAll();

    [[nodiscard]] std::vector<PluginInfo> plugins() const;

    [[nodiscard]] std::size_t count() const noexcept { return loaded_.size(); }

    /// The conventional extension for a plugin on this platform.
    [[nodiscard]] static std::string_view librarySuffix() noexcept;

private:
    struct Loaded;

    CommandRegistry* registry_;
    HostDocument* document_;
    std::vector<std::unique_ptr<Loaded>> loaded_;
};

}  // namespace cad::abi
