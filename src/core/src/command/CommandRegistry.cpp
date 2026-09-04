#include "cad/command/CommandRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "cad/db/SymbolTable.hpp"

namespace cad {
namespace {

[[nodiscard]] std::string normalize(std::string_view name) {
    return normalizeSymbolName(name);
}

/// Subsequence match score: every character of `needle` must appear in
/// `haystack` in order. Consecutive matches score higher, so "ln" ranks "LiNe"
/// above a scattered match.
[[nodiscard]] int fuzzyScore(std::string_view needle, std::string_view haystack) {
    int score = 0;
    std::size_t haystackIndex = 0;
    int consecutive = 0;

    for (const char c : needle) {
        const std::size_t found = haystack.find(c, haystackIndex);
        if (found == std::string_view::npos) {
            return 0;
        }
        consecutive = (found == haystackIndex) ? consecutive + 1 : 0;
        score += 10 + consecutive * 5;
        // A match at a word boundary is more meaningful than one mid-word.
        if (found == 0 || haystack[found - 1] == ' ') {
            score += 15;
        }
        haystackIndex = found + 1;
    }
    // Prefer shorter targets among equally good matches.
    return std::max(1, score - static_cast<int>(haystack.size()));
}

}  // namespace

CommandRegistration CommandRegistry::registerCommand(CommandDefinition command) {
    if (byId_.contains(command.id)) {
        throw std::invalid_argument("A command with id '" + command.id +
                                    "' is already registered");
    }

    std::vector<std::string> names;
    names.push_back(command.name);
    names.insert(names.end(), command.aliases.begin(), command.aliases.end());

    for (const std::string& name : names) {
        const std::string key = normalize(name);
        const auto existing = byName_.find(key);
        if (existing != byName_.end()) {
            throw std::invalid_argument("Command name '" + name + "' is already used by " +
                                        existing->second);
        }
    }

    const std::string id = command.id;
    byId_[id] = std::make_unique<CommandDefinition>(std::move(command));
    for (const std::string& name : names) {
        byName_[normalize(name)] = id;
    }

    return CommandRegistration{[this, id] { unregisterCommand(id); }};
}

CommandRegistration CommandRegistry::registerLenient(std::vector<CommandDefinition> commands,
                                                     std::vector<std::string>& problems) {
    std::vector<std::string> registered;
    registered.reserve(commands.size());

    for (CommandDefinition& command : commands) {
        const std::string id = command.id;
        const std::string name = command.name;
        try {
            registerCommand(std::move(command)).release();
            registered.push_back(id);
        } catch (const std::exception& error) {
            problems.emplace_back(name + ": " + error.what());
        }
    }

    return CommandRegistration{[this, registered] {
        for (const std::string& id : registered) {
            unregisterCommand(id);
        }
    }};
}

CommandRegistration CommandRegistry::registerAll(std::vector<CommandDefinition> commands) {
    std::vector<std::string> registered;
    registered.reserve(commands.size());

    try {
        for (CommandDefinition& command : commands) {
            const std::string id = command.id;
            // The returned token is released immediately: rollback is handled
            // in bulk below so the batch is atomic rather than per-command.
            registerCommand(std::move(command)).release();
            registered.push_back(id);
        }
    } catch (...) {
        for (const std::string& id : registered) {
            unregisterCommand(id);
        }
        throw;
    }

    return CommandRegistration{[this, registered] {
        for (const std::string& id : registered) {
            unregisterCommand(id);
        }
    }};
}

bool CommandRegistry::unregisterCommand(const std::string& id) {
    const auto it = byId_.find(id);
    if (it == byId_.end()) {
        return false;
    }
    byId_.erase(it);
    std::erase_if(byName_, [&id](const auto& entry) { return entry.second == id; });
    return true;
}

const CommandDefinition* CommandRegistry::get(const std::string& id) const {
    const auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second.get();
}

const CommandDefinition* CommandRegistry::lookup(std::string_view nameOrAlias) const {
    const auto it = byName_.find(normalize(nameOrAlias));
    return it == byName_.end() ? nullptr : get(it->second);
}

bool CommandRegistry::addAlias(std::string_view alias, std::string_view commandNameOrId) {
    const std::string key = normalize(alias);
    if (key.empty()) {
        return false;
    }

    // Accept either the command's id or any of its current names, so a config
    // file can say `c = COPY` without knowing about `cad.modify.copy`.
    const CommandDefinition* target = lookup(commandNameOrId);
    if (target == nullptr) {
        target = get(std::string{commandNameOrId});
    }
    if (target == nullptr) {
        return false;
    }

    // A command's own name is never overridable. Without that, a bad line in a
    // config file could make a command unreachable from inside the application
    // that would have to fix it.
    if (const auto existing = byName_.find(key); existing != byName_.end()) {
        const CommandDefinition* owner = get(existing->second);
        if (owner != nullptr && normalize(owner->name) == key) {
            return false;
        }
    }

    byName_[key] = target->id;
    return true;
}

bool CommandRegistry::removeAlias(std::string_view alias) {
    const std::string key = normalize(alias);
    const auto it = byName_.find(key);
    if (it == byName_.end()) {
        return false;
    }
    const CommandDefinition* owner = get(it->second);
    if (owner != nullptr && normalize(owner->name) == key) {
        return false;
    }
    byName_.erase(it);
    return true;
}

std::vector<std::string> CommandRegistry::aliasesFor(const std::string& id) const {
    std::vector<std::string> out;
    const CommandDefinition* command = get(id);
    if (command == nullptr) {
        return out;
    }
    const std::string canonical = normalize(command->name);
    for (const auto& [name, target] : byName_) {
        if (target == id && name != canonical) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<const CommandDefinition*> CommandRegistry::all() const {
    std::vector<const CommandDefinition*> result;
    result.reserve(byId_.size());
    for (const auto& [id, command] : byId_) {
        result.push_back(command.get());
    }
    std::sort(result.begin(), result.end(),
              [](const CommandDefinition* a, const CommandDefinition* b) {
                  return a->name < b->name;
              });
    return result;
}

std::map<std::string, std::vector<const CommandDefinition*>> CommandRegistry::byCategory() const {
    std::map<std::string, std::vector<const CommandDefinition*>> groups;
    for (const auto& [id, command] : byId_) {
        const std::string category = command->category.empty() ? "Other" : command->category;
        groups[category].push_back(command.get());
    }
    for (auto& [category, list] : groups) {
        std::sort(list.begin(), list.end(),
                  [](const CommandDefinition* a, const CommandDefinition* b) {
                      return a->title < b->title;
                  });
    }
    return groups;
}

std::vector<CommandMatch> CommandRegistry::search(std::string_view query,
                                                  std::size_t limit) const {
    const std::string key = normalize(query);

    std::vector<CommandMatch> matches;
    if (key.empty()) {
        for (const CommandDefinition* command : all()) {
            matches.push_back({command, CommandMatch::Kind::Fuzzy, 0, command->title});
            if (matches.size() >= limit) {
                break;
            }
        }
        return matches;
    }

    for (const auto& [id, commandPtr] : byId_) {
        const CommandDefinition* command = commandPtr.get();

        std::vector<std::string> names;
        names.push_back(command->name);
        names.insert(names.end(), command->aliases.begin(), command->aliases.end());

        std::optional<CommandMatch> best;
        for (const std::string& name : names) {
            const std::string normalized = normalize(name);
            const bool isPrimary = name == command->name;

            if (normalized == key) {
                // An exact alias must still outrank a prefix match on another
                // command's full name, hence the high alias score.
                const int score = isPrimary ? 1000 : 900;
                const auto kind =
                    isPrimary ? CommandMatch::Kind::Exact : CommandMatch::Kind::Alias;
                if (!best || score > best->score) {
                    best = CommandMatch{command, kind, score, name};
                }
                continue;
            }

            if (normalized.starts_with(key)) {
                // Shorter names are better prefix matches: "LI" prefers LINE
                // over LINETYPE.
                const int score =
                    500 - static_cast<int>(normalized.size() - key.size());
                if (!best || score > best->score) {
                    best = CommandMatch{command, CommandMatch::Kind::Prefix, score, name};
                }
            }
        }

        if (!best) {
            const int score = fuzzyScore(key, normalize(command->title));
            if (score > 0) {
                best = CommandMatch{command, CommandMatch::Kind::Fuzzy, score, command->title};
            }
        }

        if (best) {
            matches.push_back(*best);
        }
    }

    std::sort(matches.begin(), matches.end(), [](const CommandMatch& a, const CommandMatch& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.command->name < b.command->name;
    });

    if (matches.size() > limit) {
        matches.resize(limit);
    }
    return matches;
}

std::string CommandRegistry::autocomplete(std::string_view partial) const {
    const std::string key = normalize(partial);
    if (key.empty()) {
        return {};
    }

    std::string best;
    for (const auto& [name, id] : byName_) {
        if (!name.starts_with(key)) {
            continue;
        }
        const CommandDefinition* command = get(id);
        if (command == nullptr) {
            continue;
        }
        if (best.empty() || command->name.size() < best.size()) {
            best = command->name;
        }
    }
    return best;
}

}  // namespace cad
