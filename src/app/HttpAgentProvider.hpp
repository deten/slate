#pragma once

#include <QString>

#include "cad/agent/AgentProvider.hpp"

namespace cad {
class ConfigFile;
}

namespace cad::app {

/// Which shape of API to speak.
///
/// Two are enough to reach almost everything. The chat-completions shape is
/// what OpenAI defined and what nearly every other service and local runner
/// copied, so "a custom endpoint speaking that shape" covers self-hosted models
/// and the aggregators without needing a plugin for each.
enum class AgentApi : std::uint8_t {
    Anthropic,
    ChatCompletions,
};

/// How to reach a provider: everything except the request itself.
struct AgentSettings {
    bool enabled = false;
    AgentApi api = AgentApi::Anthropic;
    QString endpoint;
    QString model;
    /// Never logged, never written to the command history, and redacted from
    /// error text before it is shown. A key that leaks into a log file is the
    /// most likely way this feature costs somebody money.
    QString key;
    int timeoutSeconds = 60;

    /// Defaults for an API, so choosing a provider fills in a working endpoint
    /// and model rather than an empty box.
    [[nodiscard]] static AgentSettings defaultsFor(AgentApi api);

    [[nodiscard]] static AgentSettings load(const ConfigFile& config);
    void save(ConfigFile& config) const;

    /// Whether a request could be attempted at all.
    [[nodiscard]] bool usable() const;

    /// Why not, in words a user can act on.
    [[nodiscard]] QString unusableReason() const;
};

/// Talks to a provider over HTTP.
///
/// Synchronous: a request is sent and the answer waited for. That is the honest
/// shape for something the user has just asked for and is waiting on, and it
/// keeps the console path and the window path identical. The window puts up a
/// cancellable wait around it rather than pretending the call is instant.
class HttpAgentProvider final : public AgentProvider {
public:
    explicit HttpAgentProvider(AgentSettings settings);
    ~HttpAgentProvider() override;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] AgentReply send(const AgentRequest& request) override;

private:
    AgentSettings settings_;
};

/// Remove anything key-shaped from text on its way to a log or a dialog.
///
/// Providers quote the offending key back in some error bodies, and that text
/// otherwise goes straight into the command history, which is on screen and
/// gets pasted into bug reports.
[[nodiscard]] QString redactKey(QString text, const QString& key);

}  // namespace cad::app
