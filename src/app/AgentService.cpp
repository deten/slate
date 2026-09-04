#include "AgentService.hpp"

#include <QFile>
#include <QFileInfo>

#include "Document.hpp"
#include "cad/agent/AgentProvider.hpp"
#include "cad/config/ConfigFile.hpp"

namespace cad::app {

CannedAgentProvider::CannedAgentProvider(QString path) : path_(std::move(path)) {}

std::string CannedAgentProvider::name() const {
    return QStringLiteral("canned reply from %1").arg(QFileInfo{path_}.fileName()).toStdString();
}

AgentReply CannedAgentProvider::send(const AgentRequest& request) {
    last_ = request;

    AgentReply reply;
    QFile file{path_};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        reply.error = QStringLiteral("Could not read the canned reply at %1").arg(path_)
                          .toStdString();
        return reply;
    }
    reply.text = QString::fromUtf8(file.readAll()).toStdString();
    return reply;
}

AgentService::AgentService(ConfigFile& config)
    : config_(config), settings_(AgentSettings::load(config)) {}

void AgentService::setSettings(const AgentSettings& settings) {
    settings_ = settings;
    settings_.save(config_);
}

std::unique_ptr<AgentProvider> AgentService::makeProvider() const {
    if (!canned_.isEmpty()) {
        return std::make_unique<CannedAgentProvider>(canned_);
    }
    return std::make_unique<HttpAgentProvider>(settings_);
}

AgentOutcome AgentService::propose(const QString& instruction, Document& document) {
    AgentOutcome outcome;
    if (instruction.trimmed().isEmpty()) {
        outcome.error = QObject::tr("Nothing was asked.");
        return outcome;
    }
    // A canned reply is a scripted run and needs no key; a real one does, and
    // saying so up front beats a network error that means the same thing.
    if (canned_.isEmpty() && !settings_.usable()) {
        outcome.error = settings_.unusableReason();
        return outcome;
    }

    const std::unique_ptr<AgentProvider> provider = makeProvider();
    outcome.log << QObject::tr("Asking %1...")
                       .arg(QString::fromStdString(provider->name()));

    std::string error;
    const std::vector<Handle> selection = document.selection();
    outcome.plan = requestAgentPlan(*provider, instruction.toStdString(), document.db(),
                                    document.commands(), selection, &error);
    if (!error.empty()) {
        outcome.error = redactKey(QString::fromStdString(error), settings_.key);
        return outcome;
    }

    if (!outcome.plan.summary.empty()) {
        outcome.log << QString::fromStdString(outcome.plan.summary);
    }
    for (const std::string& refused : outcome.plan.refused) {
        outcome.log << QObject::tr("! Refused: %1").arg(QString::fromStdString(refused));
    }
    if (outcome.plan.empty()) {
        outcome.error = outcome.plan.refused.empty()
                            ? QObject::tr("The assistant proposed no changes.")
                            : QObject::tr("Nothing the assistant proposed can be run here.");
    }
    return outcome;
}

void AgentService::apply(AgentOutcome& outcome, Document& document) {
    if (outcome.plan.empty()) {
        return;
    }
    outcome.run = applyAgentPlan(document.executor(), outcome.plan);
    for (const std::string& line : outcome.run.log) {
        outcome.log << QString::fromStdString(line);
    }
    outcome.log << QObject::tr("%1 of %2 step(s) completed.")
                       .arg(outcome.run.completed)
                       .arg(outcome.run.completed + outcome.run.failed);
    document.reindex();
}

}  // namespace cad::app
