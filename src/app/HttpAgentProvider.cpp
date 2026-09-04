#include "HttpAgentProvider.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include "cad/config/ConfigFile.hpp"

namespace cad::app {
namespace {

constexpr const char* kSection = "agent";

[[nodiscard]] QString apiName(AgentApi api) {
    return api == AgentApi::Anthropic ? QStringLiteral("anthropic")
                                      : QStringLiteral("chat-completions");
}

[[nodiscard]] AgentApi apiFromName(const QString& name) {
    return name.compare(QLatin1String{"anthropic"}, Qt::CaseInsensitive) == 0
               ? AgentApi::Anthropic
               : AgentApi::ChatCompletions;
}

/// The text out of whichever reply shape came back.
///
/// Both APIs bury the answer somewhere different, and both have a habit of
/// returning a perfectly valid document that contains an error instead. A
/// missing field is reported as such rather than read as an empty answer: an
/// empty answer looks like a model with nothing to say, which is a different
/// problem with a different fix.
[[nodiscard]] AgentReply readReply(AgentApi api, const QByteArray& body) {
    AgentReply reply;
    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse);
    if (document.isNull() || !document.isObject()) {
        reply.error = QStringLiteral("The provider did not return JSON: %1")
                          .arg(parse.errorString())
                          .toStdString();
        return reply;
    }
    const QJsonObject root = document.object();

    if (root.contains(QStringLiteral("error"))) {
        const QJsonValue error = root.value(QStringLiteral("error"));
        const QString message = error.isObject()
                                    ? error.toObject()
                                          .value(QStringLiteral("message"))
                                          .toString()
                                    : error.toString();
        reply.error = (message.isEmpty() ? QStringLiteral("The provider reported an error")
                                         : message)
                          .toStdString();
        return reply;
    }

    QString text;
    if (api == AgentApi::Anthropic) {
        for (const QJsonValue& piece : root.value(QStringLiteral("content")).toArray()) {
            const QJsonObject block = piece.toObject();
            if (block.value(QStringLiteral("type")).toString() == QLatin1String{"text"}) {
                text += block.value(QStringLiteral("text")).toString();
            }
        }
    } else {
        const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
        if (!choices.isEmpty()) {
            text = choices.first()
                       .toObject()
                       .value(QStringLiteral("message"))
                       .toObject()
                       .value(QStringLiteral("content"))
                       .toString();
        }
    }

    if (text.isEmpty()) {
        reply.error = "The provider returned no text. The model or endpoint may be wrong for "
                      "the selected API.";
        return reply;
    }
    reply.text = text.toStdString();
    return reply;
}

}  // namespace

AgentSettings AgentSettings::defaultsFor(AgentApi api) {
    AgentSettings settings;
    settings.api = api;
    if (api == AgentApi::Anthropic) {
        settings.endpoint = QStringLiteral("https://api.anthropic.com/v1/messages");
        settings.model = QStringLiteral("claude-sonnet-4-5");
    } else {
        settings.endpoint = QStringLiteral("https://api.openai.com/v1/chat/completions");
        settings.model = QStringLiteral("gpt-4o");
    }
    return settings;
}

AgentSettings AgentSettings::load(const ConfigFile& config) {
    const AgentApi api =
        apiFromName(QString::fromStdString(config.text(kSection, "api", "anthropic")));
    AgentSettings settings = defaultsFor(api);
    settings.enabled = config.boolean(kSection, "enabled", false);
    if (const auto endpoint = config.get(kSection, "endpoint"); endpoint && !endpoint->empty()) {
        settings.endpoint = QString::fromStdString(*endpoint);
    }
    if (const auto model = config.get(kSection, "model"); model && !model->empty()) {
        settings.model = QString::fromStdString(*model);
    }
    settings.timeoutSeconds =
        static_cast<int>(config.integer(kSection, "timeout.seconds", 60));
    settings.referenceDirectory =
        QString::fromStdString(config.text(kSection, "reference.directory", ""));

    // The environment wins over the file. Somebody who would rather not leave a
    // key on disk can export it instead, and nothing here writes it back.
    if (const char* fromEnvironment = std::getenv("SLATE_AGENT_KEY");
        fromEnvironment != nullptr && *fromEnvironment != '\0') {
        settings.key = QString::fromUtf8(fromEnvironment);
    } else {
        settings.key = QString::fromStdString(config.text(kSection, "key", ""));
    }
    return settings;
}

void AgentSettings::save(ConfigFile& config) const {
    config.setBoolean(kSection, "enabled", enabled);
    config.set(kSection, "api", apiName(api).toStdString());
    config.set(kSection, "endpoint", endpoint.toStdString());
    config.set(kSection, "model", model.toStdString());
    config.set(kSection, "timeout.seconds", std::to_string(timeoutSeconds));
    config.set(kSection, "reference.directory", referenceDirectory.toStdString());
    // Only written when it came from the box. A key supplied through the
    // environment is left there rather than being copied onto disk behind the
    // back of somebody who deliberately kept it off disk.
    if (std::getenv("SLATE_AGENT_KEY") == nullptr) {
        config.set(kSection, "key", key.toStdString());
    }
}

bool AgentSettings::usable() const {
    return enabled && !endpoint.isEmpty() && !model.isEmpty() && !key.isEmpty();
}

QString AgentSettings::unusableReason() const {
    if (!enabled) {
        return QObject::tr("The assistant is turned off. Turn it on in Tools > AI Assistant.");
    }
    if (key.isEmpty()) {
        return QObject::tr("No API key. Add one in Tools > AI Assistant, or set SLATE_AGENT_KEY.");
    }
    if (endpoint.isEmpty() || model.isEmpty()) {
        return QObject::tr("The assistant needs an endpoint and a model.");
    }
    return {};
}

QString redactKey(QString text, const QString& key) {
    if (!key.isEmpty()) {
        text.replace(key, QStringLiteral("<key>"));
    }
    return text;
}

HttpAgentProvider::HttpAgentProvider(AgentSettings settings) : settings_(std::move(settings)) {}

HttpAgentProvider::~HttpAgentProvider() = default;

std::string HttpAgentProvider::name() const {
    return QStringLiteral("%1 (%2)").arg(settings_.model, apiName(settings_.api)).toStdString();
}

AgentReply HttpAgentProvider::send(const AgentRequest& request) {
    AgentReply reply;
    if (!settings_.usable()) {
        reply.error = settings_.unusableReason().toStdString();
        return reply;
    }

    const QString instruction =
        QStringLiteral("%1\n\nThe request:\n%2")
            .arg(QString::fromStdString(request.context),
                 QString::fromStdString(request.instruction));

    QJsonObject body;
    body.insert(QStringLiteral("model"), settings_.model);
    QNetworkRequest http{QUrl{settings_.endpoint}};
    http.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    if (settings_.api == AgentApi::Anthropic) {
        body.insert(QStringLiteral("max_tokens"), 2048);
        body.insert(QStringLiteral("system"), QString::fromStdString(request.system));
        QJsonObject message;
        message.insert(QStringLiteral("role"), QStringLiteral("user"));
        message.insert(QStringLiteral("content"), instruction);
        body.insert(QStringLiteral("messages"), QJsonArray{message});
        http.setRawHeader("x-api-key", settings_.key.toUtf8());
        http.setRawHeader("anthropic-version", "2023-06-01");
    } else {
        QJsonObject system;
        system.insert(QStringLiteral("role"), QStringLiteral("system"));
        system.insert(QStringLiteral("content"), QString::fromStdString(request.system));
        QJsonObject message;
        message.insert(QStringLiteral("role"), QStringLiteral("user"));
        message.insert(QStringLiteral("content"), instruction);
        body.insert(QStringLiteral("messages"), QJsonArray{system, message});
        http.setRawHeader("Authorization", "Bearer " + settings_.key.toUtf8());
    }

    QNetworkAccessManager network;
    QNetworkReply* response =
        network.post(http, QJsonDocument{body}.toJson(QJsonDocument::Compact));

    // Waited on with a timer, so a provider that never answers ends the wait
    // rather than the application. Without it a wrong endpoint hangs the window
    // with no way back.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        response->abort();
    });
    QObject::connect(response, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(std::max(1, settings_.timeoutSeconds) * 1000);
    loop.exec();
    timeout.stop();

    const QByteArray payload = response->readAll();
    const QNetworkReply::NetworkError error = response->error();
    const QString errorText = response->errorString();
    response->deleteLater();

    if (timedOut) {
        reply.error = QObject::tr("The provider did not answer within %1 seconds.")
                          .arg(settings_.timeoutSeconds)
                          .toStdString();
        return reply;
    }
    if (error != QNetworkReply::NoError) {
        // The body first: a provider explains a refused key there, and the
        // transport only says "protocol error".
        AgentReply parsed = readReply(settings_.api, payload);
        const QString detail = parsed.error.empty() ? errorText
                                                    : QString::fromStdString(parsed.error);
        reply.error = redactKey(detail, settings_.key).toStdString();
        return reply;
    }

    reply = readReply(settings_.api, payload);
    if (!reply.ok()) {
        reply.error = redactKey(QString::fromStdString(reply.error), settings_.key).toStdString();
    }
    return reply;
}

}  // namespace cad::app
