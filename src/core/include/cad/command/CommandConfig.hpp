#pragma once

#include <string>
#include <vector>

#include "cad/command/CommandRegistry.hpp"
#include "cad/config/ConfigFile.hpp"

namespace cad {

/// The configuration section holding user command aliases.
inline constexpr const char* kAliasSection = "aliases";

/// The section holding keyboard shortcuts, keyed by command name.
inline constexpr const char* kShortcutSection = "shortcuts";

/// What happened when a configuration was applied.
///
/// Reported rather than thrown: a typo in one alias must not cost the user the
/// other forty, and a silent failure would leave them typing an alias that does
/// nothing with no way to find out why.
struct ConfigApplyResult {
    /// Aliases successfully bound.
    int applied = 0;
    /// One message per line that could not be used, naming the line.
    std::vector<std::string> problems;

    [[nodiscard]] bool ok() const noexcept { return problems.empty(); }
};

/// Bind every alias in the config's `[aliases]` section to its command.
///
/// Entries are `alias = COMMAND`. A user alias overrides a built-in one of the
/// same name, which is the point: remapping `C` from CIRCLE to COPY is a common
/// and reasonable preference.
[[nodiscard]] ConfigApplyResult applyAliases(CommandRegistry& registry,
                                             const ConfigFile& config);

/// A keyboard shortcut bound to a command.
struct ShortcutBinding {
    /// Command name as written in the config, for reporting.
    std::string command;
    /// Command id, resolved.
    std::string commandId;
    /// Key sequence in the portable `Ctrl+Shift+L` spelling.
    std::string keys;
};

/// Read the `[shortcuts]` section, resolving each command name to an id.
///
/// Returns the bindings rather than applying them, because binding a key is a
/// UI concern and the core must not know what a key event is.
[[nodiscard]] std::vector<ShortcutBinding> readShortcuts(const CommandRegistry& registry,
                                                         const ConfigFile& config,
                                                         ConfigApplyResult& result);

/// Write a starter configuration explaining itself.
///
/// Generated only when no file exists. A configuration file a user is expected
/// to hand-edit should arrive documented rather than empty, or the feature is
/// undiscoverable outside the manual.
[[nodiscard]] std::string defaultConfigText();

}  // namespace cad
