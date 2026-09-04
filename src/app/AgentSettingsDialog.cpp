#include "AgentSettingsDialog.hpp"

#include <cstdlib>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace cad::app {

AgentSettingsDialog::AgentSettingsDialog(AgentSettings settings, QWidget* parent)
    : QDialog(parent), settings_(std::move(settings)) {
    setWindowTitle(tr("AI Assistant"));
    setModal(true);
    setMinimumWidth(560);

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

    // The office's drawings, which are where its conventions live.
    reference_ = new QLineEdit{settings_.referenceDirectory, this};
    reference_->setPlaceholderText(tr("A folder of your drawings"));
    auto* browse = new QPushButton{tr("Browse..."), this};
    auto* show = new QPushButton{tr("Show"), this};
    show->setToolTip(tr("Read the folder and show what the assistant learns from it."));
    auto* referenceRow = new QHBoxLayout;
    referenceRow->addWidget(reference_, 1);
    referenceRow->addWidget(browse);
    referenceRow->addWidget(show);
    form->addRow(tr("Reference folder"), referenceRow);

    auto* referenceNote = new QLabel{
        tr("The drawings in this folder are read once and the assistant learns your layer "
           "names, colours, blocks and styles from them, so what it adds matches how you "
           "already work. Notes in .txt or .md files there are passed along as written."),
        this};
    referenceNote->setWordWrap(true);
    form->addRow(QString{}, referenceNote);

    auto* buttons =
        new QDialogButtonBox{QDialogButtonBox::Save | QDialogButtonBox::Cancel, this};
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(api_, &QComboBox::currentIndexChanged, this,
            [this] { applyApiDefaults(); });
    connect(browse, &QPushButton::clicked, this, &AgentSettingsDialog::browseReference);
    connect(show, &QPushButton::clicked, this, &AgentSettingsDialog::showStandards);
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

void AgentSettingsDialog::browseReference() {
    const QString chosen =
        QFileDialog::getExistingDirectory(this, tr("Reference folder"), reference_->text());
    if (!chosen.isEmpty()) {
        reference_->setText(chosen);
    }
}

void AgentSettingsDialog::showStandards() {
    const QString directory = reference_->text().trimmed();
    QString text;
    if (directory.isEmpty()) {
        text = tr("Choose a folder first.");
    } else if (preview_) {
        text = preview_(directory);
    }
    if (text.isEmpty()) {
        text = tr("Nothing was learned from %1.").arg(directory);
    }

    QDialog viewer{this};
    viewer.setWindowTitle(tr("What the assistant learned"));
    viewer.resize(720, 520);
    auto* viewerLayout = new QVBoxLayout{&viewer};
    auto* body = new QPlainTextEdit{text, &viewer};
    body->setReadOnly(true);
    body->setLineWrapMode(QPlainTextEdit::NoWrap);
    viewerLayout->addWidget(body);
    auto* close = new QDialogButtonBox{QDialogButtonBox::Close, &viewer};
    connect(close, &QDialogButtonBox::rejected, &viewer, &QDialog::reject);
    connect(close, &QDialogButtonBox::accepted, &viewer, &QDialog::accept);
    viewerLayout->addWidget(close);
    viewer.exec();
}

AgentSettings AgentSettingsDialog::settings() const {
    AgentSettings result = settings_;
    result.enabled = enabled_->isChecked();
    result.api = static_cast<AgentApi>(api_->currentData().toInt());
    result.endpoint = endpoint_->text().trimmed();
    result.model = model_->text().trimmed();
    result.key = key_->text().trimmed();
    result.timeoutSeconds = timeout_->value();
    result.referenceDirectory = reference_->text().trimmed();
    return result;
}

}  // namespace cad::app
