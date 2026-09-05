#include "PlotSetupDialog.hpp"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "cad/config/ConfigFile.hpp"

namespace cad::app {
namespace {

constexpr const char* kSection = "plot";

/// The scales offered, as paper millimetres per drawing unit for a drawing in
/// millimetres. Named as a drafter names them.
struct NamedScale {
    const char* name;
    double paperPerUnit;
};
constexpr NamedScale kScales[] = {
    {"Fit to page", 0.0},   {"1:1", 1.0},       {"1:2", 0.5},       {"1:5", 0.2},
    {"1:10", 0.1},          {"1:20", 0.05},     {"1:25", 0.04},     {"1:50", 0.02},
    {"1:100", 0.01},        {"1:200", 0.005},   {"1:500", 0.002},   {"2:1", 2.0},
};

}  // namespace

PlotSetup PlotSetup::load(const ConfigFile& config) {
    PlotSetup setup;
    setup.area = config.text(kSection, "area", "extents") == "window" ? Area::Window : Area::Extents;
    setup.window = Box3::fromCorners(
        Vec3{config.number(kSection, "window.left", 0.0), config.number(kSection, "window.bottom", 0.0), 0.0},
        Vec3{config.number(kSection, "window.right", 0.0), config.number(kSection, "window.top", 0.0), 0.0});
    setup.paperPerUnit = config.number(kSection, "scale", 0.0);
    setup.monochrome = config.boolean(kSection, "monochrome", true);
    return setup;
}

void PlotSetup::save(ConfigFile& config) const {
    config.set(kSection, "area", area == Area::Window ? "window" : "extents");
    config.setNumber(kSection, "window.left", window.isEmpty() ? 0.0 : window.min.x);
    config.setNumber(kSection, "window.bottom", window.isEmpty() ? 0.0 : window.min.y);
    config.setNumber(kSection, "window.right", window.isEmpty() ? 0.0 : window.max.x);
    config.setNumber(kSection, "window.top", window.isEmpty() ? 0.0 : window.max.y);
    config.setNumber(kSection, "scale", paperPerUnit);
    config.setBoolean(kSection, "monochrome", monochrome);
}

PlotSetupDialog::PlotSetupDialog(PlotSetup setup, bool onSheet, QWidget* parent)
    : QDialog(parent), setup_(setup), onSheet_(onSheet) {
    setWindowTitle(tr("Plot Setup"));
    setModal(true);
    setMinimumWidth(440);

    auto* layout = new QVBoxLayout{this};
    auto* form = new QFormLayout;
    layout->addLayout(form);

    area_ = new QComboBox{this};
    area_->addItem(tr("Everything"), static_cast<int>(PlotSetup::Area::Extents));
    area_->addItem(tr("Window"), static_cast<int>(PlotSetup::Area::Window));
    area_->setCurrentIndex(setup_.area == PlotSetup::Area::Window ? 1 : 0);
    form->addRow(tr("Plot area"), area_);

    const auto spin = [this](double value) {
        auto* box = new QDoubleSpinBox{this};
        box->setRange(-1e9, 1e9);
        box->setDecimals(3);
        box->setValue(value);
        return box;
    };
    const bool haveWindow = !setup_.window.isEmpty();
    left_ = spin(haveWindow ? setup_.window.min.x : 0.0);
    bottom_ = spin(haveWindow ? setup_.window.min.y : 0.0);
    right_ = spin(haveWindow ? setup_.window.max.x : 0.0);
    top_ = spin(haveWindow ? setup_.window.max.y : 0.0);
    auto* corners = new QHBoxLayout;
    corners->addWidget(new QLabel{tr("from"), this});
    corners->addWidget(left_);
    corners->addWidget(bottom_);
    corners->addWidget(new QLabel{tr("to"), this});
    corners->addWidget(right_);
    corners->addWidget(top_);
    form->addRow(tr("Window"), corners);

    scale_ = new QComboBox{this};
    int chosen = -1;
    for (const NamedScale& option : kScales) {
        scale_->addItem(tr(option.name), option.paperPerUnit);
        if (std::abs(option.paperPerUnit - setup_.paperPerUnit) < 1e-12) {
            chosen = scale_->count() - 1;
        }
    }
    scale_->addItem(tr("Custom"), -1.0);
    scale_->setCurrentIndex(chosen >= 0 ? chosen : scale_->count() - 1);
    customScale_ = new QDoubleSpinBox{this};
    customScale_->setRange(0.000001, 1000.0);
    customScale_->setDecimals(6);
    customScale_->setSuffix(tr(" mm per unit"));
    customScale_->setValue(setup_.paperPerUnit > 0.0 ? setup_.paperPerUnit : 1.0);
    auto* scaleRow = new QHBoxLayout;
    scaleRow->addWidget(scale_, 1);
    scaleRow->addWidget(customScale_);
    form->addRow(tr("Scale"), scaleRow);

    monochrome_ = new QCheckBox{tr("Plot in black"), this};
    monochrome_->setChecked(setup_.monochrome);
    form->addRow(QString{}, monochrome_);

    note_ = new QLabel{this};
    note_->setWordWrap(true);
    note_->setText(onSheet_ ? tr("A sheet plots at its own size: the scale is set by its "
                                 "viewports and page setup, so it is not asked here. The "
                                 "window is in sheet millimetres.")
                            : tr("Scales are paper millimetres per drawing unit, so 1:100 in "
                                 "a millimetre drawing puts one metre in a centimetre. Fit to "
                                 "page ignores the scale. Lineweights plot at their width."));
    form->addRow(QString{}, note_);

    auto* buttons = new QDialogButtonBox{QDialogButtonBox::Save | QDialogButtonBox::Cancel, this};
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(area_, &QComboBox::currentIndexChanged, this, [this] { refreshEnabled(); });
    connect(scale_, &QComboBox::currentIndexChanged, this, [this] { refreshEnabled(); });
    refreshEnabled();
}

void PlotSetupDialog::refreshEnabled() {
    const bool window = area_->currentData().toInt() == static_cast<int>(PlotSetup::Area::Window);
    for (QDoubleSpinBox* box : {left_, bottom_, right_, top_}) {
        box->setEnabled(window);
    }
    scale_->setEnabled(!onSheet_);
    customScale_->setEnabled(!onSheet_ && scale_->currentData().toDouble() < 0.0);
}

PlotSetup PlotSetupDialog::setup() const {
    PlotSetup result = setup_;
    result.area = static_cast<PlotSetup::Area>(area_->currentData().toInt());
    result.window = Box3::fromCorners(Vec3{left_->value(), bottom_->value(), 0.0},
                                      Vec3{right_->value(), top_->value(), 0.0});
    const double named = scale_->currentData().toDouble();
    result.paperPerUnit = named < 0.0 ? customScale_->value() : named;
    result.monochrome = monochrome_->isChecked();
    return result;
}

}  // namespace cad::app
