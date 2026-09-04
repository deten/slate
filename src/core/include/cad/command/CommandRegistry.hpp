#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cad/command/Command.hpp"

namespace cad {

/// A command matched against typed text, with its match quality.
struct CommandMatch {
    enum class Kind : std::uint8_t { Exact, Alias, Prefix, Fuzzy };

    const CommandDefinition* command = nullptr;
    Kind kind = Kind::Fuzzy;
    /// Relevance for ranking; higher is better.
    int score = 0;
    /// Which string matched, for highlighting in the palette.
    std::string matched;
};

/// A token that unregisters a command when destroyed or released.
///
/// Returned by `registerCommand`, which is what makes plugin unloading possible
/// without the registry having to track ownership.
class CommandRegistration {
public:
    CommandRegistration() = default;
    CommandRegistration(std::function<void()> undo) : undo_(std::move(undo)) {}

    CommandRegistration(const CommandRegistration&) = delete;
    CommandRegistration& operator=(const CommandRegistration&) = delete;

    CommandRegistration(CommandRegistration&& other) noexcept
        : undo_(std::exchange(other.undo_, nullptr)) {}

    CommandRegistration& operator=(CommandRegistration&& other) noexcept {
        if (this != &other) {
            reset();
            undo_ = std::exchange(other.undo_, nullptr);
        }
        return *this;
    }

    ~CommandRegistration() { reset(); }

    void reset() {
        if (undo_) {
            undo_();
            undo_ = nullptr;
        }
    }

    /// Keep the registration permanently, abandoning the ability to undo it.
    void release() noexcept { undo_ = nullptr; }

private:
    std::function<void()> undo_;
};

/// The catalogue of available commands.
///
/// A registry rather than a fixed list, because the brief requires custom
/// commands, macros and plugins to be first-class and addable without modifying
/// the engine. Every consumer — the command line, the palette, menus, keyboard
/// shortcuts, and later the scripting API — reads from here, so a registered
/// command is automatically available everywhere.
///
/// Names and aliases are case-insensitive, matching other CAD software.
class CommandRegistry {
public:
    /// Register a command.
    /// @throws std::invalid_argument when the id, name or an alias is taken.
    ///   Silent overwriting would let one plugin shadow another invisibly.
    [[nodiscard]] CommandRegistration registerCommand(CommandDefinition command);

    /// Register several atomically: a colliding batch rolls back entirely.
    [[nodiscard]] CommandRegistration registerAll(std::vector<CommandDefinition> commands);

    /// Register as many as fit, reporting what would not.
    ///
    /// Deliberately *not* atomic, unlike `registerAll`. That one is for a
    /// plugin, where a half-registered library is worse than none. This one is
    /// for commands the user wrote: a macro file naming itself LINE must cost
    /// the user that file and no others, and certainly not the rest of the
    /// folder. Each refusal is appended to `problems` saying which name clashed
    /// with what.
    [[nodiscard]] CommandRegistration registerLenient(std::vector<CommandDefinition> commands,
                                                      std::vector<std::string>& problems);

    bool unregisterCommand(const std::string& id);

    [[nodiscard]] const CommandDefinition* get(const std::string& id) const;

    /// Look up by name or alias, case-insensitively.
    [[nodiscard]] const CommandDefinition* lookup(std::string_view nameOrAlias) const;

    /// Point an alias at a command, replacing any existing alias of that name.
    ///
    /// Unlike registration, this deliberately *overrides*: a user who remaps `C`
    /// from CIRCLE to COPY — which many other CAD software users do — is expressing a
    /// preference, not making a mistake, and refusing it would make the feature
    /// useless for the people most likely to want it.
    ///
    /// A command's own registered *name* can never be shadowed, so typing MOVE
    /// always runs MOVE however the aliases are configured. Losing that would
    /// make a misconfigured file impossible to recover from inside the
    /// application.
    ///
    /// @returns false when the target command does not exist, or when the alias
    ///   would shadow a command name.
    bool addAlias(std::string_view alias, std::string_view commandNameOrId);

    /// Remove an alias. Refuses to remove a command's own name.
    bool removeAlias(std::string_view alias);

    /// Every alias pointing at a command, normalized to upper case and sorted.
    /// The command's own name is excluded, since it is not an alias.
    [[nodiscard]] std::vector<std::string> aliasesFor(const std::string& id) const;

    [[nodiscard]] std::size_t size() const noexcept { return byId_.size(); }

    [[nodiscard]] std::vector<const CommandDefinition*> all() const;

    /// Commands grouped by category, for building menus.
    [[nodiscard]] std::map<std::string, std::vector<const CommandDefinition*>> byCategory() const;

    /// Rank commands against typed text for the command line and palette.
    ///
    /// The ordering is deliberate and matters for muscle memory: an exact name
    /// always wins, then an exact alias, then a prefix, then a subsequence
    /// match. A user who types `L` must get LINE — because `L` is its
    /// registered alias — and never a fuzzy match that happens to score higher.
    [[nodiscard]] std::vector<CommandMatch> search(std::string_view query,
                                                   std::size_t limit = 20) const;

    /// Autocomplete suggestion: the shortest command whose name or alias begins
    /// with what the user has typed. The *canonical* name is suggested, not the
    /// alias that matched, so the user learns the real command name.
    [[nodiscard]] std::string autocomplete(std::string_view partial) const;

private:
    std::unordered_map<std::string, std::unique_ptr<CommandDefinition>> byId_;
    /// Normalized name or alias -> command id.
    std::unordered_map<std::string, std::string> byName_;
};

}  // namespace cad
