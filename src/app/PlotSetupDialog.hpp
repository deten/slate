#pragma once

#include <optional>

#include <QDialog>

#include "cad/math/Box3.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace cad {
class ConfigFile;
}

namespace cad::app {

/// What a plot shows and at what size: the settings behind File > Plot and
/// Export PDF, kept in the settings file.
struct PlotSetup {
    enum class Area { Extents, Window };

    Area area = Area::Extents;
    Box3 window;
    /// Millimetres of paper per drawing unit for a model plot; zero fits.
    double paperPerUnit = 0.0;
    bool monochrome = true;

    [[nodiscard]] static PlotSetup load(const ConfigFile& config);
    void save(ConfigFile& config) const;
};

/// Plot Setup: the area, the scale and the ink.
///
/// Sheets plot at their own size, so the scale here is for a model plot; on
/// a sheet the box is disabled and says so.
class PlotSetupDialog final : public QDialog {
    Q_OBJECT

public:
    PlotSetupDialog(PlotSetup setup, bool onSheet, QWidget* parent = nullptr);

    [[nodiscard]] PlotSetup setup() const;

private:
    void refreshEnabled();

    QComboBox* area_ = nullptr;
    QDoubleSpinBox* left_ = nullptr;
    QDoubleSpinBox* bottom_ = nullptr;
    QDoubleSpinBox* right_ = nullptr;
    QDoubleSpinBox* top_ = nullptr;
    QComboBox* scale_ = nullptr;
    QDoubleSpinBox* customScale_ = nullptr;
    QCheckBox* monochrome_ = nullptr;
    QLabel* note_ = nullptr;
    PlotSetup setup_;
    bool onSheet_ = false;
};

}  // namespace cad::app
