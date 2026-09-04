#include "SettingsDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace cad::app {

SettingsDialog::SettingsDialog(ConfigFile& config, std::function<void()> onApply,
                               QWidget* parent)
    : QDialog{parent}, config_(config), onApply_(std::move(onApply)) {
    setWindowTitle(tr("Settings"));
    setMinimumWidth(460);

    auto* outer = new QVBoxLayout{this};

    auto* files = new QGroupBox{tr("Files"), this};
    auto* filesForm = new QFormLayout{files};
    autoSaveEnabled_ = new QCheckBox{tr("Auto-save a recovery copy"), files};
    autoSaveMinutes_ = new QDoubleSpinBox{files};
    autoSaveMinutes_->setRange(0.1, 120.0);
    autoSaveMinutes_->setDecimals(1);
    autoSaveMinutes_->setSuffix(tr(" minutes"));
    backupEnabled_ = new QCheckBox{tr("Keep the previous file as .bak when saving"), files};
    filesForm->addRow(autoSaveEnabled_);
    filesForm->addRow(tr("Every"), autoSaveMinutes_);
    filesForm->addRow(backupEnabled_);
    // Says where, because a recovery copy nobody can find is not a recovery.
    auto* where = new QLabel{
        tr("Recovery copies are written beside your configuration, never over your drawing."),
        files};
    where->setWordWrap(true);
    filesForm->addRow(where);
    outer->addWidget(files);

    auto* drafting = new QGroupBox{tr("Drafting"), this};
    auto* draftingForm = new QFormLayout{drafting};
    aperturePixels_ = new QSpinBox{drafting};
    aperturePixels_->setRange(2, 40);
    aperturePixels_->setSuffix(tr(" pixels"));
    aperturePixels_->setToolTip(
        tr("How close a click has to be to catch an object, and how close the "
           "cursor has to be for a snap."));
    crosshairPercent_ = new QSpinBox{drafting};
    crosshairPercent_->setRange(1, 100);
    crosshairPercent_->setSuffix(tr(" %"));
    crosshairPercent_->setToolTip(
        tr("Length of the crosshair arms as a percentage of the drawing area. "
           "100 spans it entirely, which helps line work up across a sheet."));
    textGreeking_ = new QSpinBox{drafting};
    textGreeking_->setRange(1, 20);
    textGreeking_->setSuffix(tr(" pixels"));
    textGreeking_->setToolTip(
        tr("Text smaller than this is drawn as a bar rather than as characters, "
           "which is other CAD software's QTEXT. Set it to 1 to always draw the characters, "
           "however small they become."));
    imageFrames_ = new QCheckBox{tr("Frame attached images"), drafting};
    imageFrames_->setToolTip(
        tr("Draw a border around raster images, which is other CAD software's IMAGEFRAME. "
           "Off by default because the border plots; turn it on to make an "
           "image easy to pick. An image whose file is missing is always framed."));
    rolloverHighlight_ = new QCheckBox{tr("Outline the object under the cursor"), drafting};
    draftingForm->addRow(tr("Pick aperture"), aperturePixels_);
    draftingForm->addRow(tr("Crosshair size"), crosshairPercent_);
    draftingForm->addRow(tr("Simplify text below"), textGreeking_);
    draftingForm->addRow(imageFrames_);
    draftingForm->addRow(rolloverHighlight_);
    auto* cursorNote =
        new QLabel{tr("The box at the crosshair is the aperture: a click or a snap "
                      "catches whatever falls inside it."),
                   drafting};
    cursorNote->setWordWrap(true);
    draftingForm->addRow(cursorNote);
    outer->addWidget(drafting);

    auto* plotting = new QGroupBox{tr("Plotting"), this};
    auto* plotForm = new QFormLayout{plotting};
    plotMonochrome_ = new QCheckBox{tr("Plot in black and white"), plotting};
    plotMonochrome_->setToolTip(
        tr("A drawing drawn against a dark background is mostly white, and "
           "white ink is no ink."));
    plotForm->addRow(plotMonochrome_);
    outer->addWidget(plotting);

    auto* customization = new QGroupBox{tr("Customization"), this};
    auto* customLayout = new QVBoxLayout{customization};
    auto* note = new QLabel{
        tr("Command aliases and keyboard shortcuts are kept in a text file you "
           "can edit directly. Comments and ordering are preserved."),
        customization};
    note->setWordWrap(true);
    auto* edit = new QPushButton{tr("Edit Aliases and Shortcuts…"), customization};
    connect(edit, &QPushButton::clicked, this, [this] {
        Q_EMIT openConfigFileRequested();
        accept();
    });
    customLayout->addWidget(note);
    customLayout->addWidget(edit);
    outer->addWidget(customization);

    auto* buttons = new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this};
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        store();
        if (onApply_) {
            onApply_();
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    connect(autoSaveEnabled_, &QCheckBox::toggled, autoSaveMinutes_, &QWidget::setEnabled);

    load();
}

void SettingsDialog::load() {
    // Defaults match the ones the application uses when the key is absent, so
    // opening the dialog and pressing OK changes nothing.
    autoSaveEnabled_->setChecked(config_.boolean("files", "autosave.enabled", true));
    autoSaveMinutes_->setValue(config_.number("files", "autosave.minutes", 10.0));
    autoSaveMinutes_->setEnabled(autoSaveEnabled_->isChecked());
    backupEnabled_->setChecked(config_.boolean("files", "backup.enabled", true));
    plotMonochrome_->setChecked(config_.boolean("plot", "monochrome", true));
    aperturePixels_->setValue(static_cast<int>(config_.integer("drafting", "aperture", 10)));
    crosshairPercent_->setValue(static_cast<int>(config_.integer("drafting", "crosshair", 5)));
    textGreeking_->setValue(static_cast<int>(config_.integer("drafting", "text.greeking", 3)));
    imageFrames_->setChecked(config_.boolean("drafting", "image.frames", false));
    rolloverHighlight_->setChecked(config_.boolean("drafting", "rollover", true));
}

void SettingsDialog::store() {
    config_.setBoolean("files", "autosave.enabled", autoSaveEnabled_->isChecked());
    config_.setNumber("files", "autosave.minutes", autoSaveMinutes_->value());
    config_.setBoolean("files", "backup.enabled", backupEnabled_->isChecked());
    config_.setBoolean("plot", "monochrome", plotMonochrome_->isChecked());
    config_.set("drafting", "aperture", std::to_string(aperturePixels_->value()));
    config_.set("drafting", "crosshair", std::to_string(crosshairPercent_->value()));
    config_.set("drafting", "text.greeking", std::to_string(textGreeking_->value()));
    config_.set("drafting", "image.frames", imageFrames_->isChecked() ? "true" : "false");
    config_.setBoolean("drafting", "rollover", rolloverHighlight_->isChecked());
}

}  // namespace cad::app
