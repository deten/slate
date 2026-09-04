#include "cad/command/CommandConfig.hpp"

namespace cad {

ConfigApplyResult applyAliases(CommandRegistry& registry, const ConfigFile& config) {
    ConfigApplyResult result;

    for (const auto& [alias, command] : config.entries(kAliasSection)) {
        if (command.empty()) {
            result.problems.push_back("Alias \"" + alias + "\" names no command.");
            continue;
        }
        if (registry.lookup(command) == nullptr && registry.get(command) == nullptr) {
            result.problems.push_back("Alias \"" + alias + "\": no command named \"" + command +
                                      "\".");
            continue;
        }
        if (!registry.addAlias(alias, command)) {
            // The only remaining refusal is shadowing a command's own name.
            result.problems.push_back("Alias \"" + alias +
                                      "\" is the name of a command and cannot be reused.");
            continue;
        }
        ++result.applied;
    }
    return result;
}

std::vector<ShortcutBinding> readShortcuts(const CommandRegistry& registry,
                                           const ConfigFile& config,
                                           ConfigApplyResult& result) {
    std::vector<ShortcutBinding> bindings;

    for (const auto& [command, keys] : config.entries(kShortcutSection)) {
        if (keys.empty()) {
            result.problems.push_back("Shortcut for \"" + command + "\" is empty.");
            continue;
        }
        const CommandDefinition* definition = registry.lookup(command);
        if (definition == nullptr) {
            definition = registry.get(command);
        }
        if (definition == nullptr) {
            result.problems.push_back("Shortcut \"" + keys + "\": no command named \"" +
                                      command + "\".");
            continue;
        }
        bindings.push_back({command, definition->id, keys});
    }
    return bindings;
}

std::string defaultConfigText() {
    return
        "# Slate configuration\n"
        "#\n"
        "# Edited by hand or from Settings. Comments, ordering and anything the\n"
        "# application does not recognise are preserved when it saves.\n"
        "\n"
        "[aliases]\n"
        "# One-word shortcuts for the command line: `alias = COMMAND`.\n"
        "# These override the built-in aliases, so remapping C from CIRCLE to\n"
        "# COPY works as you would expect.\n"
        "#\n"
        "# c = COPY\n"
        "# zz = ZOOM\n"
        "\n"
        "[shortcuts]\n"
        "# Key sequences, written as `COMMAND = Ctrl+Shift+L`.\n"
        "#\n"
        "# LINE = Ctrl+Shift+L\n"
        "# ERASE = Del\n";
}

}  // namespace cad
