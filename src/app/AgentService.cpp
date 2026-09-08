#include "AgentService.hpp"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include "Document.hpp"
#include "ProjectIndex.hpp"
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
    // One file may script a whole exchange: replies separated by a line of
    // three or more equals signs, served in order as the conversation grows.
    // The last one is repeated if the exchange runs longer than the script.
    const QStringList replies = QString::fromUtf8(file.readAll())
                                    .split(QRegularExpression{QStringLiteral("(?m)^={3,}\\s*$")});
    int modelTurns = 0;
    for (const AgentTurn& turn : request.history) {
        modelTurns += turn.fromModel ? 1 : 0;
    }
    const int index = std::min(modelTurns, static_cast<int>(replies.size()) - 1);
    reply.text = replies.value(std::max(index, 0)).toStdString();
    return reply;
}

AgentService::AgentService(ConfigFile& config, const io::DwgTranslator* translator,
                           QString cacheDirectory)
    : config_(config),
      settings_(AgentSettings::load(config)),
      library_(translator, std::move(cacheDirectory)) {}

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

QString AgentService::referenceDirectory() const {
    return referenceOverride_.isEmpty() ? settings_.referenceDirectory : referenceOverride_;
}

QString AgentService::standards(QStringList* log) {
    const QString directory = referenceDirectory();
    if (directory.trimmed().isEmpty()) {
        return {};
    }
    const ReferenceLibrary::Result result = library_.read(directory);
    if (log != nullptr) {
        if (!result.empty() && !result.fromCache) {
            *log << QObject::tr("Read %n drawing(s) from %1.", "", result.drawings)
                        .arg(QDir::toNativeSeparators(directory));
        }
        for (const QString& problem : result.problems) {
            *log << QObject::tr("! %1").arg(problem);
        }
    }
    return result.text;
}

QString AgentService::standardsPreview(const QString& directory) {
    const ReferenceLibrary::Result result = library_.read(directory);
    QString text = result.text;
    if (!result.problems.isEmpty()) {
        if (!text.isEmpty()) {
            text += QLatin1Char('\n');
        }
        text += result.problems.join(QLatin1Char('\n'));
    }
    return text;
}

QString AgentService::context(Document& document, QStringList* log) {
    const std::vector<Handle> selection = document.selection();
    QString text = QString::fromStdString(describeDrawing(document.db(), selection));
    const QString set = projectDigest(document, log);
    if (!set.isEmpty()) {
        text += QLatin1Char('\n');
        text += set;
    }
    const QString house = standards(log);
    if (!house.isEmpty()) {
        text += QLatin1Char('\n');
        text += house;
    }
    return text;
}

QString AgentService::projectDigest(Document& document, QStringList* log) {
    if (project_ == nullptr || !project_->isOpen()) {
        return {};
    }
    QStringList problems;
    QString text = project_->digest(&problems);
    if (const ProjectSheet* sheet = project_->sheetForFile(QString::fromStdString(document.filePath));
        sheet != nullptr) {
        text += QStringLiteral("The open drawing is sheet %1 %2.\n")
                    .arg(QString::fromStdString(sheet->number),
                         QString::fromStdString(sheet->title));
    }
    if (log != nullptr) {
        for (const QString& problem : problems) {
            *log << QObject::tr("  project: %1").arg(problem);
        }
    }
    return text;
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

    // The set goes in with the office's standards: what the other sheets
    // hold is as much a convention as which layer a wall goes on.
    std::string house = projectDigest(document, &outcome.log).toStdString();
    const std::string conventions = standards(&outcome.log).toStdString();
    if (!conventions.empty()) {
        house += (house.empty() ? "" : "\n") + conventions;
    }
    std::string error;
    const std::vector<Handle> selection = document.selection();
    // The back-and-forth: facts answered from the drawing, questions put to
    // whoever is set up to answer them, and all of it logged so the history
    // shows what the assistant asked and what it was told.
    std::vector<std::string> transcript;
    AgentDialogue dialogue;
    dialogue.answer = questions_;
    if (project_ != nullptr) {
        dialogue.beyondTheDrawing = [this](const AgentQuery& query) {
            return project_->answer(query);
        };
    }
    dialogue.transcript = &transcript;
    outcome.plan = requestAgentPlan(*provider, instruction.toStdString(), document.db(),
                                    document.commands(), selection, &error, house, &dialogue);
    for (const std::string& line : transcript) {
        outcome.log << QString::fromStdString(line);
    }
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
