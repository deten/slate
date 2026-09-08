#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cad/db/Database.hpp"

namespace cad {

struct DrawingReading;

/// A question the model put to the user, with the answer it would assume.
struct AgentQuestion {
    std::string text;
    std::string suggested;
};

/// A question the model put to Slate about the drawing: "ASK ROOM CONF. #1".
struct AgentQuery {
    /// The line as written.
    std::string source;
    /// ROOM, ROOMS, LAYER, NEAR or BLOCK, in capitals.
    std::string kind;
    /// The rest of the line.
    std::string argument;
};

/// What a reply turned out to be, besides commands: the questions it asks
/// the user, and the facts it asks Slate for.
struct AgentTurnReading {
    std::vector<AgentQuestion> questions;
    std::vector<AgentQuery> queries;

    [[nodiscard]] bool asksTheUser() const noexcept { return !questions.empty(); }
    [[nodiscard]] bool asksTheDrawing() const noexcept { return !queries.empty(); }
};

/// Read the questions and queries out of a reply.
///
/// A question is a line starting with "?", with the suggested answer in
/// square brackets at the end; a query is a line starting with ASK. Both are
/// read wherever they appear, fenced or not, because a model asked to write
/// a fenced block sometimes writes its questions outside it.
[[nodiscard]] AgentTurnReading readAgentTurn(std::string_view reply);

/// Answer one query from the drawing, in prose the model can use.
///
/// The reading is what DESCRIBE reports, passed in so it is made once per
/// request rather than once per question.
[[nodiscard]] std::string answerAgentQuery(const Database& db, const DrawingReading& reading,
                                           const AgentQuery& query);

/// How the user is asked. Given the questions, returns one answer per
/// question, or nothing when the user would rather not go on.
using AgentAnswerer =
    std::function<std::optional<std::vector<std::string>>(const std::vector<AgentQuestion>&)>;

/// The back-and-forth a request is allowed.
///
/// A model that can ask for facts stays cheap on a large drawing, because
/// the request carries a summary and the model fetches the rest; one that
/// can ask the user a question stops guessing at what only the user knows.
/// Both are bounded: a few fact rounds, and one round of questions, so a
/// model that keeps asking cannot keep the user waiting forever.
struct AgentDialogue {
    /// How to put questions to the user. Without one, each question is
    /// taken as answered with its suggestion, which is what a scripted run
    /// wants.
    AgentAnswerer answer;
    /// How many times the model may ask Slate for facts before it must answer.
    int factRounds = 4;
    /// Answers for the questions the drawing alone cannot: about the
    /// project and its other sheets, which live in files the core does not
    /// open. Given a query, returns the answer, or nothing to let the
    /// drawing's own answerer have it.
    std::function<std::optional<std::string>(const AgentQuery&)> beyondTheDrawing;
    /// Everything said either way after the first request, for the history.
    std::vector<std::string>* transcript = nullptr;
};

}  // namespace cad
