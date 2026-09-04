#pragma once

#include <QDialog>
#include <vector>

#include "cad/db/Purge.hpp"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace cad::app {

class Document;

/// Choose what to remove from a drawing that nothing refers to any more.
///
/// A list rather than a yes-or-no question, because "unused" and "unwanted" are
/// different things and only the person who made the drawing knows which is
/// which. A set of PDFs imported to trace over is unused the moment the tracing
/// is done and may still be wanted next week; the anonymous block left behind by
/// an edit never was.
///
/// Grouped by kind and checked by default, so the common answer -- all of it --
/// is one button, and the uncommon one is a few clicks rather than a different
/// command.
class PurgeDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PurgeDialog(Document& document, QWidget* parent = nullptr);

private:
    void reload();
    void onPurge();
    void onSelectionChanged();
    void setAllChecked(bool checked);

    /// What the user has ticked, in the order `findPurgeable` reported it.
    [[nodiscard]] std::vector<PurgeCandidate> checkedCandidates() const;

    Document& document_;
    QLabel* summary_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPushButton* purge_ = nullptr;
    std::vector<PurgeCandidate> candidates_;
};

}  // namespace cad::app
