#pragma once

#include <functional>

#include <QDialog>

#include "HttpAgentProvider.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace cad::app {

/// Where a user points the application at their own assistant.
///
/// Deliberately not a list of blessed vendors. Two request shapes cover
/// essentially everything -- Anthropic's, and the chat-completions shape that
/// OpenAI defined and everyone else copied -- so a service this application has
/// never heard of, or a model running on the machine next to it, works by
/// typing its address in.
class AgentSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    AgentSettingsDialog(AgentSettings settings, QWidget* parent = nullptr);

    /// How to show what a reference folder teaches. Given a folder, returns
    /// the text the assistant would be handed.
    void setStandardsPreview(std::function<QString(const QString&)> preview) {
        preview_ = std::move(preview);
    }

    [[nodiscard]] AgentSettings settings() const;

private:
    /// Fill the endpoint and model with something that works for the chosen
    /// API, but only when the user has not typed their own: silently replacing
    /// a self-hosted address with a vendor's would be worse than an empty box.
    void applyApiDefaults();
    void refreshKeyNote();
    void browseReference();
    void showStandards();

    QCheckBox* enabled_ = nullptr;
    QComboBox* api_ = nullptr;
    QLineEdit* endpoint_ = nullptr;
    QLineEdit* model_ = nullptr;
    QLineEdit* key_ = nullptr;
    QSpinBox* timeout_ = nullptr;
    QLabel* keyNote_ = nullptr;
    QLineEdit* reference_ = nullptr;
    std::function<QString(const QString&)> preview_;
    AgentSettings settings_;
};

}  // namespace cad::app
