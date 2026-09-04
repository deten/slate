#pragma once

#include <memory>

#include <QString>
#include <QStringList>

#include "HttpAgentProvider.hpp"
#include "cad/agent/AgentPlan.hpp"

namespace cad {
class ConfigFile;
}

namespace cad::app {

class Document;

/// A provider that answers from a file instead of a network.
///
/// This is how the whole application path -- the command, the drawing summary,
/// the reply, what is allowed to run, and the edit it makes -- is tested
/// without a key or a socket. It is reachable from the command line rather than
/// only from a test, because a scripted run *is* the test: `--agent-reply`
/// names a file holding what a model would have said.
///
/// It is not a way to hide that the network path is untested. What this covers
/// is everything either side of the transport; the transport itself is checked
/// by pointing the real provider at a real endpoint, which no automated run
/// here does.
class CannedAgentProvider final : public AgentProvider {
public:
    explicit CannedAgentProvider(QString path);

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] AgentReply send(const AgentRequest& request) override;

    /// The last request built, so a scripted run can show what would have been
    /// sent. Being able to read the prompt without paying for it is most of
    /// what makes this worth having.
    [[nodiscard]] const AgentRequest& lastRequest() const noexcept { return last_; }

private:
    QString path_;
    AgentRequest last_;
};

/// What an agent run produced, for reporting.
struct AgentOutcome {
    AgentPlan plan;
    AgentRunResult run;
    QString error;
    /// Lines to put in the command history, in order, already redacted.
    QStringList log;

    [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

/// Owns the assistant: its settings, which provider to use, and running one
/// instruction against a drawing.
///
/// Deliberately not a Qt object and not tied to a window, so the console path
/// and the window path are the same code with a different way of asking
/// whether to apply the plan.
class AgentService {
public:
    explicit AgentService(ConfigFile& config);

    [[nodiscard]] const AgentSettings& settings() const noexcept { return settings_; }
    void setSettings(const AgentSettings& settings);

    /// Answer from this file rather than a provider, for scripted runs.
    void useCannedReply(const QString& path) { canned_ = path; }
    [[nodiscard]] bool isCanned() const { return !canned_.isEmpty(); }

    /// Ask for a plan. Nothing is applied.
    [[nodiscard]] AgentOutcome propose(const QString& instruction, Document& document);

    /// Run a plan that was proposed, reporting what each step did.
    void apply(AgentOutcome& outcome, Document& document);

private:
    [[nodiscard]] std::unique_ptr<AgentProvider> makeProvider() const;

    ConfigFile& config_;
    AgentSettings settings_;
    QString canned_;
};

}  // namespace cad::app
