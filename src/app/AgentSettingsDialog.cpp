#include "AgentSettingsDialog.hpp"

#include <cstdlib>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace cad::app {

AgentSettingsDialog::AgentSettingsDialog(AgentSettings settings, QWidget* parent)
    : QDialog(parent), settings_(std::move(settings)) {
    setWindowTitle(tr("AI Assistant"));
    setModal(true);

    auto* layout = new QVBoxLayout{this};

    auto* intro = new QLabel{
        tr("Describe a change in plain words and the assistant answers with drawing "
           "commands. Nothing is applied until you have seen the commands and agreed to "
           "them."),
        this};
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    layout->addLayout(form);

    enabled_ = new QCheckBox{tr("Use an AI assistant"), this};
    enabled_->setChecked(settings_.enabled);
    form->addRow(enabled_);

    api_ = new QComboBox{this};
    api_->addItem(tr("Anthropic messages"), static_cast<int>(AgentApi::Anthropic));
    api_->addItem(tr("OpenAI-compatible chat completions"),
                  static_cast<int>(AgentApi::ChatCompletions));
    api_->setCurrentIndex(settings_.api == AgentApi::Anthropic ? 0 : 1);
    api_->setToolTip(tr("The shape of the request. Most services, and most local model "
                        "runners, speak the chat-completions shape."));
    form->addRow(tr("API"), api_);

    endpoint_ = new QLineEdit{settings_.endpoint, this};
    endpoint_->setPlaceholderText(tr("https://..."));
    form->addRow(tr("Endpoint"), endpoint_);

    model_ = new QLineEdit{settings_.model, this};
    form->addRow(tr("Model"), model_);

    key_ = new QLineEdit{settings_.key, this};
    key_->setEchoMode(QLineEdit::Password);
    key_->setPlaceholderText(tr("Paste your API key"));
    form->addRow(tr("API key"), key_);

    keyNote_ = new QLabel{this};
    keyNote_->setWordWrap(true);
    form->addRow(QString{}, keyNote_);

    timeout_ = new QSpinBox{this};
    timeout_->setRange(5, 600);
    timeout_->setSuffix(tr(" seconds"));
    timeout_->setValue(settings_.timeoutSeconds);
    form->addRow(tr("Give up after"), timeout_);

    auto* buttons =
        new QDialogButtonBox{QDialogButtonBox::Save | QDialogButtonBox::Cancel, this};
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(api_, &QComboBox::currentIndexChanged, this,
            [this] { applyApiDefaults(); });
    refreshKeyNote();
}

void AgentSettingsDialog::applyApiDefaults() {
    const auto api = static_cast<AgentApi>(api_->currentData().toInt());
    const AgentSettings defaults = AgentSettings::defaultsFor(api);
    // Only into boxes the user has not made their own. Overwriting a
    // self-hosted address with a vendor's because the API selector moved would
    // undo the exact thing this dialog exists to allow.
    const AgentSettings anthropic = AgentSettings::defaultsFor(AgentApi::Anthropic);
    const AgentSettings openai = AgentSettings::defaultsFor(AgentApi::ChatCompletions);
    const bool endpointUntouched = endpoint_->text().isEmpty() ||
                                   endpoint_->text() == anthropic.endpoint ||
                                   endpoint_->text() == openai.endpoint;
    const bool modelUntouched = model_->text().isEmpty() || model_->text() == anthropic.model ||
                                model_->text() == openai.model;
    if (endpointUntouched) {
        endpoint_->setText(defaults.endpoint);
    }
    if (modelUntouched) {
        model_->setText(defaults.model);
    }
}

void AgentSettingsDialog::refreshKeyNote() {
    if (std::getenv("SLATE_AGENT_KEY") != nullptr) {
        keyNote_->setText(tr("SLATE_AGENT_KEY is set, and is used instead of anything typed "
                             "here. Nothing is written to disk."));
        key_->setEnabled(false);
        return;
    }
    // Said plainly rather than buried. Somebody handing this drawing folder to
    // a colleague should know their key is in it.
    keyNote_->setText(tr("Stored in plain text in your settings file. To keep it off disk, "
                         "set the SLATE_AGENT_KEY environment variable instead."));
}

AgentSettings AgentSettingsDialog::settings() const {
    AgentSettings result = settings_;
    result.enabled = enabled_->isChecked();
    result.api = static_cast<AgentApi>(api_->currentData().toInt());
    result.endpoint = endpoint_->text().trimmed();
    result.model = model_->text().trimmed();
    result.key = key_->text().trimmed();
    result.timeoutSeconds = timeout_->value();
    return result;
}

}  // namespace cad::app
