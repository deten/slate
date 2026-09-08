#include "test_helpers.hpp"

#include <string>
#include <vector>

#include <optional>

#include "cad/agent/AgentDialogue.hpp"
#include "cad/agent/AgentPlan.hpp"
#include "cad/agent/AgentProvider.hpp"
#include "cad/agent/DrawingReading.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/command/CommandExecutor.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Transaction.hpp"

using namespace cad;

namespace {

/// A provider that answers from a script instead of a network.
///
/// The whole point of the provider being an interface: every part of the path
/// from a sentence to an edited drawing is exercised here -- the prompt, the
/// reply format, what is allowed to run, and what running it does -- with no
/// key, no socket and no bill. What is *not* covered is the transport itself,
/// and that is stated rather than implied.
class ScriptedProvider final : public AgentProvider {
public:
    explicit ScriptedProvider(std::string reply) : reply_(std::move(reply)) {}

    [[nodiscard]] std::string name() const override { return "scripted"; }

    [[nodiscard]] AgentReply send(const AgentRequest& request) override {
        seen = request;
        ++calls;
        AgentReply reply;
        if (fail.empty()) {
            reply.text = reply_;
        } else {
            reply.error = fail;
        }
        return reply;
    }

    std::string fail;
    AgentRequest seen;
    int calls = 0;

private:
    std::string reply_;
};

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager transactions{*db};
    std::unique_ptr<CommandRegistry> registryOwner = createStandardRegistry();
    CommandRegistry& registry = *registryOwner;
    SimpleSelection selection;
    CommandExecutor executor{*db, transactions, registry, selection};

    [[nodiscard]] std::size_t entityCount() const {
        std::size_t count = 0;
        for (const Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (entity != nullptr && !entity->erased) {
                ++count;
            }
        }
        return count;
    }
};

}  // namespace

TEST_SUITE("Agent") {

TEST_CASE("commands are read out of a fenced reply") {
    const AgentPlan plan = parseAgentPlan(
        "I will draw the outline.\n"
        "```\n"
        "RECTANGLE 0,0 400,300\n"
        "CIRCLE 200,150 50\n"
        "```\n"
        "That gives you a rectangle with a circle in it.");

    REQUIRE(plan.commands.size() == 2);
    CHECK(plan.commands[0].name == "RECTANGLE");
    CHECK(plan.commands[0].arguments == std::vector<std::string>{"0,0", "400,300"});
    CHECK(plan.commands[1].name == "CIRCLE");
    // Prose from either side of the block is kept, and kept out of the plan.
    CHECK(plan.summary.find("draw the outline") != std::string::npos);
    CHECK(plan.summary.find("rectangle with a circle") != std::string::npos);
}

TEST_CASE("a reply with no fence is read as commands throughout") {
    // Told to answer in commands, a model often simply does. Refusing that for
    // want of decoration would fail for a reason the user cannot see.
    const AgentPlan plan = parseAgentPlan("LINE 0,0 100,0\nLINE 100,0 100,100\n");
    REQUIRE(plan.commands.size() == 2);
    CHECK(plan.commands[0].name == "LINE");
    CHECK(plan.commands[1].arguments.back() == "100,100");
}

TEST_CASE("blank lines, comments and casing do not matter") {
    const AgentPlan plan = parseAgentPlan(
        "```\n"
        "# the outline\n"
        "\n"
        "  rectangle 0,0 400,300  \n"
        "```");
    REQUIRE(plan.commands.size() == 1);
    CHECK(plan.commands[0].name == "RECTANGLE");
}

TEST_CASE("a command the application does not have is refused, not run") {
    // The common failure: a model inventing a plausible-sounding command.
    // Running its arguments through whatever else happened to match would
    // change the drawing in a way nobody asked for, and the reply would still
    // read as though it had worked.
    Harness h;
    AgentPlan plan = parseAgentPlan(
        "```\n"
        "LINE 0,0 100,100\n"
        "SUMMON-DRAGON 5\n"
        "```");
    REQUIRE(plan.commands.size() == 2);

    validateAgentPlan(plan, h.registry);
    REQUIRE(plan.commands.size() == 1);
    CHECK(plan.commands[0].name == "LINE");
    REQUIRE(plan.refused.size() == 1);
    CHECK(plan.refused[0].find("SUMMON-DRAGON") != std::string::npos);
}

TEST_CASE("running a plan draws what it says") {
    Harness h;
    AgentPlan plan = parseAgentPlan(
        "```\n"
        "LINE 0,0 400,0\n"
        "CIRCLE 200,150 50\n"
        "```");
    validateAgentPlan(plan, h.registry);

    const AgentRunResult result = applyAgentPlan(h.executor, plan);
    CHECK(result.ok());
    CHECK(result.completed == 2);
    CHECK(h.entityCount() == 2);

    const auto* line = dynamic_cast<const LineEntity*>(h.db->entitiesIn(h.db->modelSpace())[0]);
    REQUIRE(line != nullptr);
    CHECK(line->start.equals(Vec3{0.0, 0.0, 0.0}));
    CHECK(line->end.equals(Vec3{400.0, 0.0, 0.0}));
}

TEST_CASE("a step that fails does not take the rest of the plan with it") {
    // The drawing is left in whatever state the good steps produced, and the
    // result says which failed. Stopping half way leaves the same partial state
    // with less explanation.
    Harness h;
    AgentPlan plan = parseAgentPlan(
        "```\n"
        "LINE 0,0 400,0\n"
        "CIRCLE not-a-point 50\n"
        "LINE 0,100 400,100\n"
        "```");
    validateAgentPlan(plan, h.registry);

    const AgentRunResult result = applyAgentPlan(h.executor, plan);
    CHECK_FALSE(result.ok());
    CHECK(result.completed == 2);
    CHECK(result.failed == 1);
    CHECK(h.entityCount() == 2);
    // The log names the step and what went wrong with it.
    const bool explained = std::any_of(result.log.begin(), result.log.end(),
                                       [](const std::string& line) {
                                           return line.find("not-a-point") != std::string::npos;
                                       });
    CHECK(explained);
}

TEST_CASE("a plan leaves no command running behind it") {
    // LINE keeps asking for points, so it needs the Enter a person would give
    // it. Left running, the next step's name would be swallowed as a point --
    // and an Enter at the bare prompt repeats the last command, so giving one
    // unconditionally would run every step twice.
    Harness h;
    AgentPlan plan = parseAgentPlan("```\nLINE 0,0 400,0\nCIRCLE 0,0 10\n```");
    validateAgentPlan(plan, h.registry);

    const AgentRunResult result = applyAgentPlan(h.executor, plan);
    CHECK(result.completed == 2);
    CHECK_FALSE(h.executor.isRunning());
    CHECK(h.entityCount() == 2);
}

TEST_CASE("the prompt offers the commands that actually exist") {
    Harness h;
    const std::string prompt = agentSystemPrompt(h.registry);
    CHECK(prompt.find("LINE") != std::string::npos);
    CHECK(prompt.find("CIRCLE") != std::string::npos);
    // Generated from the registry rather than written down, so it cannot drift
    // from what the application has.
    CHECK(prompt.find("SUMMON-DRAGON") == std::string::npos);
}

TEST_CASE("the drawing description says what is there") {
    Harness h;
    h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{400.0, 0.0, 0.0}));
    h.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3{200.0, 150.0, 0.0}, 50.0));

    const std::string context = describeDrawing(*h.db);
    CHECK(context.find("2 objects") != std::string::npos);
    CHECK(context.find("Line") != std::string::npos);
    CHECK(context.find("Circle") != std::string::npos);
    CHECK(context.find("Extents") != std::string::npos);
    CHECK(context.find("Current layer") != std::string::npos);
}

TEST_CASE("an empty drawing says so rather than reporting empty extents") {
    Harness h;
    const std::string context = describeDrawing(*h.db);
    CHECK(context.find("empty") != std::string::npos);
}

TEST_CASE("a request carries the instruction, the drawing and the rules") {
    Harness h;
    ScriptedProvider provider{"```\nLINE 0,0 10,10\n```"};
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "draw a diagonal", *h.db, h.registry, {},
                                            &error);

    CHECK(provider.calls == 1);
    CHECK(provider.seen.instruction == "draw a diagonal");
    CHECK(provider.seen.system.find("Available commands") != std::string::npos);
    CHECK(provider.seen.context.find("Current layer") != std::string::npos);
    CHECK(error.empty());
    REQUIRE(plan.commands.size() == 1);
}

TEST_CASE("a provider that fails is reported, not treated as an empty plan") {
    // A refused key and a model with nothing to suggest are different answers,
    // and a user who has just pasted a key needs to be told which.
    Harness h;
    ScriptedProvider provider{""};
    provider.fail = "401 Unauthorized";
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "draw a diagonal", *h.db, h.registry, {},
                                            &error);
    CHECK(plan.empty());
    CHECK(error == "401 Unauthorized");
}

TEST_CASE("a whole exchange edits the drawing") {
    // End to end through the seam a real provider plugs into.
    Harness h;
    ScriptedProvider provider{
        "Here is a 400 by 300 room.\n"
        "```\n"
        "RECTANGLE 0,0 400,300\n"
        "```"};

    std::string error;
    AgentPlan plan = requestAgentPlan(provider, "draw a 400 by 300 room", *h.db, h.registry, {},
                                      &error);
    REQUIRE(error.empty());
    REQUIRE(plan.commands.size() == 1);

    const AgentRunResult result = applyAgentPlan(h.executor, plan);
    CHECK(result.ok());
    CHECK(h.entityCount() == 1);

    const auto* shape =
        dynamic_cast<const PolylineEntity*>(h.db->entitiesIn(h.db->modelSpace())[0]);
    REQUIRE(shape != nullptr);
    CHECK(shape->closed);
    CHECK(shape->geometry().area() == doctest::Approx(120000.0));
}

TEST_CASE("everything an agent does can be undone") {
    // It can only issue commands, so it can only do what the user could have
    // done by hand -- which is the property that makes handing it a drawing
    // reasonable at all. Each step is its own undo, as if typed.
    Harness h;
    AgentPlan plan = parseAgentPlan("```\nLINE 0,0 400,0\nCIRCLE 0,0 10\n```");
    validateAgentPlan(plan, h.registry);
    applyAgentPlan(h.executor, plan);
    REQUIRE(h.entityCount() == 2);

    CHECK(h.transactions.undo());
    CHECK(h.entityCount() == 1);
    CHECK(h.transactions.undo());
    CHECK(h.entityCount() == 0);
}

// ----------------------------------------------------------------- dialogue

namespace {

/// A provider that answers from a list, one reply per call, and keeps every
/// request it was sent so the conversation can be checked turn by turn.
class ConversingProvider final : public AgentProvider {
public:
    explicit ConversingProvider(std::vector<std::string> replies) : replies_(std::move(replies)) {}
    [[nodiscard]] std::string name() const override { return "conversing"; }
    [[nodiscard]] AgentReply send(const AgentRequest& request) override {
        requests.push_back(request);
        AgentReply reply;
        const std::size_t index = std::min(requests.size() - 1, replies_.size() - 1);
        reply.text = replies_[index];
        return reply;
    }
    std::vector<AgentRequest> requests;

private:
    std::vector<std::string> replies_;
};

/// A drawing with one enclosed, labelled room and a duct run.
struct RoomHarness : Harness {
    RoomHarness() {
        db->settings.insertionUnits = DrawingUnits::Inches;
        Layer& walls = db->ensureLayer("A-WALL");
        const Vec3 corners[] = {{0, 0, 0}, {400, 0, 0}, {400, 300, 0}, {0, 300, 0}};
        for (int i = 0; i < 4; ++i) {
            auto line = std::make_unique<LineEntity>(corners[i], corners[(i + 1) % 4]);
            line->layerHandle = walls.handle;
            db->addToModelSpace(std::move(line));
        }
        db->addToModelSpace(std::make_unique<TextEntity>(Vec3{200, 160, 0}, "OFFICE", 10.0));
        db->addToModelSpace(std::make_unique<TextEntity>(Vec3{200, 140, 0}, "101", 10.0));
        Layer& duct = db->ensureLayer("M-DUCT-SUPP");
        const Vec3 run[] = {{50, 250, 0}, {350, 250, 0}};
        auto polyline = std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(run));
        polyline->constantWidth = 12.0;
        polyline->layerHandle = duct.handle;
        db->addToModelSpace(std::move(polyline));
    }
};

}  // namespace

TEST_CASE("questions and queries are read out of a reply, fenced or not") {
    const AgentTurnReading turn = readAgentTurn(
        "I need to know more.\n"
        "```\n"
        "ASK ROOM CONF. #1\n"
        "ask layer M-DUCT-SUPP\n"
        "```\n"
        "? Which room is the mechanical room? [MECH. EQT. RM 6]\n"
        "? What size is the main?\n");
    REQUIRE(turn.queries.size() == 2);
    CHECK(turn.queries[0].kind == "ROOM");
    CHECK(turn.queries[0].argument == "CONF. #1");
    CHECK(turn.queries[1].kind == "LAYER");
    CHECK(turn.queries[1].argument == "M-DUCT-SUPP");
    REQUIRE(turn.questions.size() == 2);
    CHECK(turn.questions[0].text == "Which room is the mechanical room?");
    CHECK(turn.questions[0].suggested == "MECH. EQT. RM 6");
    CHECK(turn.questions[1].text == "What size is the main?");
    CHECK(turn.questions[1].suggested.empty());

    const AgentTurnReading plain = readAgentTurn("```\nLINE 0,0 1,1\n```\n");
    CHECK_FALSE(plain.asksTheUser());
    CHECK_FALSE(plain.asksTheDrawing());
}

TEST_CASE("a query is answered from the drawing") {
    RoomHarness h;
    const DrawingReading reading = readDrawing(*h.db);

    const std::string room = answerAgentQuery(*h.db, reading, AgentQuery{"ASK ROOM office", "ROOM", "office"});
    CHECK(room.find("OFFICE 101: 833 sq ft") != std::string::npos);
    CHECK(room.find("Bounded by 4 objects") != std::string::npos);
    CHECK(room.find("Line from 0,0 to 400,0") != std::string::npos);
    // The duct crosses the room, so it is inside.
    CHECK(room.find("Inside (1): 1 x Polyline") != std::string::npos);

    const std::string missing = answerAgentQuery(*h.db, reading, AgentQuery{"ASK ROOM lab", "ROOM", "lab"});
    CHECK(missing.find("No room called \"lab\"") != std::string::npos);
    CHECK(missing.find("OFFICE 101") != std::string::npos);

    const std::string layer = answerAgentQuery(*h.db, reading, AgentQuery{"ASK LAYER a-wall", "LAYER", "a-wall"});
    CHECK(layer.find("On layer a-wall (4): 4 x Line") != std::string::npos);

    const std::string near = answerAgentQuery(*h.db, reading, AgentQuery{"ASK NEAR 0,0 10", "NEAR", "0,0 10"});
    CHECK(near.find("Within 10 of 0,0 (2)") != std::string::npos);

    const std::string bad = answerAgentQuery(*h.db, reading, AgentQuery{"ASK WEATHER", "WEATHER", ""});
    CHECK(bad.find("Unknown question") != std::string::npos);

    const std::string rooms = answerAgentQuery(*h.db, reading, AgentQuery{"ASK ROOMS", "ROOMS", ""});
    CHECK(rooms.find("Rooms (1):") != std::string::npos);
}

TEST_CASE("a model that asks for facts is answered and asked again") {
    RoomHarness h;
    ConversingProvider provider{{
        "```\nASK ROOM OFFICE\n```\n",
        "```\nLINE 0,150 400,150\n```\n",
    }};
    std::vector<std::string> transcript;
    AgentDialogue dialogue;
    dialogue.transcript = &transcript;
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "run a duct through the office", *h.db,
                                            h.registry, {}, &error, {}, &dialogue);
    CHECK(error.empty());
    REQUIRE(plan.commands.size() == 1);
    CHECK(plan.commands[0].name == "LINE");

    REQUIRE(provider.requests.size() == 2);
    // The second request carries what the model asked and what it was told.
    const AgentRequest& second = provider.requests[1];
    REQUIRE(second.history.size() == 2);
    CHECK(second.history[0].fromModel);
    CHECK(second.history[0].text.find("ASK ROOM OFFICE") != std::string::npos);
    CHECK_FALSE(second.history[1].fromModel);
    CHECK(second.history[1].text.find("OFFICE 101: 833 sq ft") != std::string::npos);
    CHECK(second.history[1].text.find("Now answer the request.") != std::string::npos);
    REQUIRE(transcript.size() == 1);
    CHECK(transcript[0] == "Asked: ASK ROOM OFFICE");
}

TEST_CASE("without a dialogue a query is refused rather than run as a command") {
    RoomHarness h;
    ConversingProvider provider{{"```\nASK ROOM OFFICE\nLINE 0,0 1,1\n```\n"}};
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "anything", *h.db, h.registry, {}, &error);
    CHECK(provider.requests.size() == 1);
    REQUIRE(plan.commands.size() == 1);
    CHECK(plan.commands[0].name == "LINE");
    REQUIRE(plan.refused.size() == 1);
    CHECK(plan.refused[0].find("ASK ROOM OFFICE") != std::string::npos);
}

TEST_CASE("the model's questions go to the user once, with suggestions as defaults") {
    RoomHarness h;
    ConversingProvider provider{{
        "? Which way should the duct run? [east to west]\n"
        "? What width? [12]\n",
        "```\nLINE 0,150 400,150\n```\n",
    }};
    std::vector<AgentQuestion> asked;
    AgentDialogue dialogue;
    dialogue.answer = [&](const std::vector<AgentQuestion>& questions) {
        asked = questions;
        // The first is answered, the second left to the suggestion.
        return std::optional<std::vector<std::string>>{{"north to south", ""}};
    };
    std::vector<std::string> transcript;
    dialogue.transcript = &transcript;
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "add a duct", *h.db, h.registry, {}, &error,
                                            {}, &dialogue);
    CHECK(error.empty());
    REQUIRE(asked.size() == 2);
    CHECK(asked[1].suggested == "12");
    REQUIRE(plan.commands.size() == 1);
    REQUIRE(provider.requests.size() == 2);
    const std::string& told = provider.requests[1].history[1].text;
    CHECK(told.find("Which way should the duct run? north to south") != std::string::npos);
    CHECK(told.find("What width? 12") != std::string::npos);
    REQUIRE(transcript.size() == 2);
    CHECK(transcript[0] == "? Which way should the duct run? -> north to south");
}

TEST_CASE("questions with no one to answer them take their suggestions") {
    RoomHarness h;
    ConversingProvider provider{{"? What width? [12]\n", "```\nLINE 0,0 1,1\n```\n"}};
    AgentDialogue dialogue;
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "add a duct", *h.db, h.registry, {}, &error,
                                            {}, &dialogue);
    CHECK(plan.commands.size() == 1);
    CHECK(provider.requests[1].history[1].text.find("What width? 12") != std::string::npos);
}

TEST_CASE("cancelling the questions cancels the request") {
    RoomHarness h;
    ConversingProvider provider{{"? What width? [12]\n", "```\nLINE 0,0 1,1\n```\n"}};
    AgentDialogue dialogue;
    dialogue.answer = [](const std::vector<AgentQuestion>&) {
        return std::optional<std::vector<std::string>>{};
    };
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "add a duct", *h.db, h.registry, {}, &error,
                                            {}, &dialogue);
    CHECK(plan.empty());
    CHECK(error.find("Cancelled") != std::string::npos);
    CHECK(provider.requests.size() == 1);
}

TEST_CASE("a second round of questions is refused, not asked") {
    RoomHarness h;
    ConversingProvider provider{{"? First? [a]\n", "? Second? [b]\n", "```\nLINE 0,0 1,1\n```\n"}};
    int askedTimes = 0;
    AgentDialogue dialogue;
    dialogue.answer = [&](const std::vector<AgentQuestion>&) {
        ++askedTimes;
        return std::optional<std::vector<std::string>>{{"a"}};
    };
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "add a duct", *h.db, h.registry, {}, &error,
                                            {}, &dialogue);
    CHECK(askedTimes == 1);
    CHECK(plan.empty());
    REQUIRE(plan.refused.size() == 1);
    CHECK(plan.refused[0] == "? Second?");
    CHECK(provider.requests.size() == 2);
}

TEST_CASE("fact rounds are bounded") {
    RoomHarness h;
    ConversingProvider provider{{"ASK ROOMS\n"}};
    AgentDialogue dialogue;
    dialogue.factRounds = 2;
    std::string error;
    const AgentPlan plan = requestAgentPlan(provider, "anything", *h.db, h.registry, {}, &error,
                                            {}, &dialogue);
    CHECK(provider.requests.size() == 3);
    CHECK(plan.empty());
    REQUIRE(plan.refused.size() == 1);
    CHECK(plan.refused[0].find("no more questions") != std::string::npos);
}

}  // TEST_SUITE
