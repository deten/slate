#pragma once

#include <QDialog>
#include <functional>

#include "cad/config/ConfigFile.hpp"

class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;

namespace cad::app {

/// Preferences.
///
/// Deliberately small, and every row in it does something. The brief lists a
/// dozen settings categories; filling a dialog with the other eleven before the
/// features behind them exist would produce controls that look configurable and
/// are not, which is worse than an honest gap — a user who discovers one
/// setting is decorative stops trusting the rest.
///
/// Values are written to the same `ConfigFile` a user can hand-edit, so nothing
/// here is a second source of truth. The file keeps its comments and its
/// ordering when this saves.
class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    /// @param config edited in place and saved by the caller's `onApply`.
    /// @param onApply invoked when settings change, so they take effect at once
    ///   rather than on the next launch. Preferences that need a restart teach
    ///   people to distrust the dialog.
    SettingsDialog(ConfigFile& config, std::function<void()> onApply,
                   QWidget* parent = nullptr);

Q_SIGNALS:
    /// The user asked to edit aliases and shortcuts by hand.
    void openConfigFileRequested();

private:
    void load();
    void store();

    ConfigFile& config_;
    std::function<void()> onApply_;

    QCheckBox* autoSaveEnabled_ = nullptr;
    QDoubleSpinBox* autoSaveMinutes_ = nullptr;
    QCheckBox* backupEnabled_ = nullptr;
    QCheckBox* plotMonochrome_ = nullptr;
    QSpinBox* aperturePixels_ = nullptr;
    QSpinBox* crosshairPercent_ = nullptr;
    QSpinBox* textGreeking_ = nullptr;
    QCheckBox* imageFrames_ = nullptr;
    QCheckBox* rolloverHighlight_ = nullptr;
};

}  // namespace cad::app
