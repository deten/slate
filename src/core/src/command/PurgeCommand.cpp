#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "cad/command/Command.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/db/Purge.hpp"
#include "cad/db/Transaction.hpp"

namespace cad {
namespace {

/// "3 blocks, 2 layers and a linetype" — what is about to go, in a sentence.
[[nodiscard]] std::string summarize(const std::vector<PurgeCandidate>& candidates) {
    std::map<std::string, std::size_t> counts;
    for (const PurgeCandidate& candidate : candidates) {
        ++counts[std::string{purgeKindName(candidate.kind)}];
    }

    std::vector<std::string> parts;
    parts.reserve(counts.size());
    for (const auto& [kind, count] : counts) {
        std::string word = kind;
        for (char& c : word) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (count > 1) {
            word += (word.back() == 'e' && word.substr(0, 4) == "styl") ? "s" : "s";
        }
        parts.push_back(std::to_string(count) + " " + word);
    }

    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            text += (i + 1 == parts.size()) ? " and " : ", ";
        }
        text += parts[i];
    }
    return text;
}

/// Remove named objects nothing refers to.
///
/// The counterpart to a drawing's history rather than to its geometry. Blocks
/// imported to trace over and then deleted, the layers that came with them, a
/// linetype used once by something since erased: none of it is reachable, all
/// of it is written to the file, and only the person who made the drawing knows
/// whether it is finished with. So this reports before it removes, and it
/// removes in one transaction so a single undo puts it all back.
class PurgeCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        // What to consider, before what is found. Asked first because the
        // answer changes the finding: keep the blocks and the layers their
        // contents sit on are still in use, so a list built for "everything"
        // and then filtered would offer layers that are not free at all.
        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Purge what";
        request.keywords = {{"All", "A"},        {"Blocks", "B"}, {"Layers", "La"},
                            {"Linetypes", "Li"}, {"Styles", "S"}};
        request.defaultText = "All";
        return StepResult::ask(request);
    }

    StepResult chooseKinds(const std::string& answer, CommandContext& context) {
        const auto matches = [&](std::string_view word, std::string_view initials) {
            return answer == word || answer == initials;
        };
        if (matches("Blocks", "B")) {
            allowed_ = PurgeKinds::Blocks;
        } else if (matches("Layers", "La")) {
            allowed_ = PurgeKinds::Layers;
        } else if (matches("Linetypes", "Li")) {
            allowed_ = PurgeKinds::Linetypes;
        } else if (matches("Styles", "S")) {
            allowed_ = PurgeKinds::Styles;
        } else {
            allowed_ = PurgeKinds::All;
        }

        candidates_ = findPurgeable(context.db(), allowed_);
        if (candidates_.empty()) {
            return StepResult::done("Nothing to purge: every named object is in use.");
        }

        std::ostringstream listing;
        listing << "Unused: " << summarize(candidates_) << ".";
        for (const PurgeCandidate& candidate : candidates_) {
            listing << "\n  " << purgeKindName(candidate.kind) << ' ' << candidate.name;
            if (candidate.kind == PurgeCandidate::Kind::Block && candidate.contents > 0) {
                listing << " (" << candidate.contents
                        << (candidate.contents == 1 ? " object)" : " objects)");
            }
        }
        context.log(listing.str());

        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Purge " + summarize(candidates_) + "?";
        request.keywords = {{"Yes", "Y"}, {"No", "N"}};
        request.defaultText = "Yes";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (!chosen_) {
            chosen_ = true;
            return chooseKinds(input.text.empty() ? "All" : input.text, context);
        }

        const std::string answer = input.text.empty() ? "Yes" : input.text;
        if (answer != "Yes" && answer != "Y" && answer != "yes" && answer != "y") {
            return StepResult::cancelled("Nothing purged.");
        }

        // Re-found rather than trusted. The list was shown to a user and the
        // drawing could have moved on while they read it, and a purge acting on
        // a stale list is a purge deleting something that came back into use.
        candidates_ = findPurgeable(context.db(), allowed_);
        const std::size_t removed = purge(context.transaction(), candidates_);
        return StepResult::done("Purged " + std::to_string(removed) + " unused item(s).");
    }

private:
    bool chosen_ = false;
    PurgeKinds allowed_ = PurgeKinds::All;
    std::vector<PurgeCandidate> candidates_;
};

}  // namespace

std::vector<CommandDefinition> purgeCommands() {
    CommandDefinition definition;
    definition.id = "cad.file.purge";
    // other CAD software's own convention: the plain name opens a dialog and the
    // hyphenated one runs at the command line. The dialog lives in the
    // application layer, so the two cannot be the same command.
    definition.name = "-PURGE";
    definition.aliases = {"-PU"};
    definition.title = "Purge (command line)";
    definition.description = "Remove blocks, layers and styles nothing uses";
    definition.category = "Tools";
    // Nothing is selected and nothing needs to be: it acts on the whole
    // drawing, and repeating it with Enter would only ever find nothing.
    definition.repeatable = false;
    definition.create = [] { return std::make_unique<PurgeCommand>(); };

    std::vector<CommandDefinition> commands;
    commands.push_back(std::move(definition));
    return commands;
}

}  // namespace cad
