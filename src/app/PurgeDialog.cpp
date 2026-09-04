#include "PurgeDialog.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <numeric>
#include <vector>
#include <map>

#include "Document.hpp"
#include "Theme.hpp"
#include "cad/db/Transaction.hpp"

namespace cad::app {
namespace {

constexpr int kIndexRole = Qt::UserRole + 1;

}  // namespace

PurgeDialog::PurgeDialog(Document& document, QWidget* parent)
    : QDialog{parent}, document_{document} {
    setWindowTitle(tr("Purge"));
    setMinimumSize(560, 420);

    auto* outer = new QVBoxLayout{this};

    summary_ = new QLabel{this};
    summary_->setWordWrap(true);
    summary_->setStyleSheet(QString{"color: %1;"}.arg(currentTheme().textMuted.name()));

    tree_ = new QTreeWidget{this};
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Name"), tr("Holds")});
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    auto* all = new QPushButton{tr("Select all"), this};
    auto* none = new QPushButton{tr("Select none"), this};
    purge_ = new QPushButton{tr("Purge"), this};
    purge_->setDefault(true);

    auto* buttons = new QDialogButtonBox{QDialogButtonBox::Close, this};
    auto* row = new QHBoxLayout;
    row->addWidget(all);
    row->addWidget(none);
    row->addStretch(1);
    row->addWidget(purge_);
    row->addWidget(buttons);

    outer->addWidget(summary_);
    outer->addWidget(tree_, 1);
    outer->addLayout(row);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(all, &QPushButton::clicked, this, [this] { setAllChecked(true); });
    connect(none, &QPushButton::clicked, this, [this] { setAllChecked(false); });
    connect(purge_, &QPushButton::clicked, this, &PurgeDialog::onPurge);
    connect(tree_, &QTreeWidget::itemChanged, this, &PurgeDialog::onSelectionChanged);

    reload();
}

void PurgeDialog::reload() {
    candidates_ = findPurgeable(document_.db());
    tree_->clear();

    // Read in an order a person can work down: the things they might recognise
    // first, the machine-named debris last. `findPurgeable` returns whatever
    // order it swept in, which put "Binder2" -- a floor plan somebody traced
    // over -- forty rows below a run of hex serial numbers.
    std::vector<std::size_t> order(candidates_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const auto anonymous = [this](std::size_t i) {
        return candidates_[i].kind == PurgeCandidate::Kind::Block &&
               isMachineNamedBlock(candidates_[i].name);
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (candidates_[a].kind != candidates_[b].kind) {
            return candidates_[a].kind < candidates_[b].kind;
        }
        if (anonymous(a) != anonymous(b)) {
            return !anonymous(a);
        }
        return candidates_[a].name < candidates_[b].name;
    });

    std::map<QString, QTreeWidgetItem*> groups;
    for (const std::size_t i : order) {
        const PurgeCandidate& candidate = candidates_[i];
        // Machine-named blocks get their own heading, so the list of names a
        // user can actually judge is not diluted by the ones they cannot.
        const QString kind =
            anonymous(i) ? tr("Anonymous block")
                         : QString::fromUtf8(purgeKindName(candidate.kind).data(),
                                             static_cast<int>(
                                                 purgeKindName(candidate.kind).size()));
        auto group = groups.find(kind);
        if (group == groups.end()) {
            auto* heading = new QTreeWidgetItem{tree_, {kind}};
            QFont font = heading->font(0);
            font.setBold(true);
            heading->setFont(0, font);
            // Not spanned: the second column carries how much the group holds,
            // which is the only thing that can be said about a folded-away run
            // of serial numbers without unfolding it.
            // Not checkable itself: a heading that could be half-ticked has to
            // explain what half-ticked means, and Select all already answers
            // the question it would be for.
            heading->setFlags(Qt::ItemIsEnabled);
            group = groups.emplace(kind, heading).first;
        }

        auto* item = new QTreeWidgetItem{group->second};
        item->setText(0, QString::fromStdString(candidate.name));
        if (candidate.kind == PurgeCandidate::Kind::Block && candidate.contents > 0) {
            // What is inside a block is the whole question for a block a user
            // might want back: an empty one is debris, and one holding three
            // hundred objects is somebody's traced-over floor plan.
            item->setText(1, candidate.contents == 1
                                 ? tr("%1 object").arg(candidate.contents)
                                 : tr("%1 objects").arg(candidate.contents));
        }
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);
        item->setData(0, kIndexRole, static_cast<qulonglong>(i));
    }
    tree_->expandAll();

    // Every heading says how much is under it, and the anonymous one arrives
    // folded away: its rows are forty serial numbers nobody reads, and the only
    // question about them is the one the count already answers.
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* heading = tree_->topLevelItem(i);
        const int held = heading->childCount();
        std::size_t objects = 0;
        for (int child = 0; child < held; ++child) {
            objects += candidates_[heading->child(child)
                                       ->data(0, kIndexRole)
                                       .toULongLong()]
                           .contents;
        }
        heading->setText(0, tr("%1 (%2)").arg(heading->text(0)).arg(held));
        if (objects > 0) {
            heading->setText(1, objects == 1 ? tr("%1 object").arg(objects)
                                             : tr("%1 objects").arg(objects));
        }
        if (heading->text(0).startsWith(tr("Anonymous block"))) {
            heading->setExpanded(false);
        }
    }

    summary_->setText(
        candidates_.empty()
            ? tr("Nothing to purge: every named object in this drawing is in use.")
            : tr("%1 named object(s) are not used by anything. Removing them changes "
                 "nothing you can see, and one undo puts them all back.")
                  .arg(candidates_.size()));
    onSelectionChanged();
}

void PurgeDialog::setAllChecked(bool checked) {
    for (QTreeWidgetItemIterator it{tree_}; *it != nullptr; ++it) {
        if (((*it)->flags() & Qt::ItemIsUserCheckable) != 0) {
            (*it)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        }
    }
}

std::vector<PurgeCandidate> PurgeDialog::checkedCandidates() const {
    std::vector<PurgeCandidate> chosen;
    for (QTreeWidgetItemIterator it{tree_}; *it != nullptr; ++it) {
        if (((*it)->flags() & Qt::ItemIsUserCheckable) == 0 ||
            (*it)->checkState(0) != Qt::Checked) {
            continue;
        }
        const auto index = static_cast<std::size_t>((*it)->data(0, kIndexRole).toULongLong());
        if (index < candidates_.size()) {
            chosen.push_back(candidates_[index]);
        }
    }
    return chosen;
}

void PurgeDialog::onSelectionChanged() {
    const std::size_t count = checkedCandidates().size();
    purge_->setEnabled(count > 0);
    purge_->setText(count == 0 ? tr("Purge") : tr("Purge %1").arg(count));
}

void PurgeDialog::onPurge() {
    const std::vector<PurgeCandidate> chosen = checkedCandidates();
    if (chosen.empty()) {
        return;
    }

    // One transaction for the whole purge, so a user who removes fifty things
    // they did not mean to needs one undo rather than fifty.
    std::size_t removed = 0;
    document_.transactions().run("Purge", [&](Transaction& transaction) {
        removed = purge(transaction, chosen);
    });

    // Re-found rather than adjusted. Removing a block can free the layer its
    // contents sat on, and the honest way to show that is to ask again.
    reload();
    if (!candidates_.empty()) {
        summary_->setText(tr("Purged %1. %2 more became unused as a result — "
                             "removing one thing is what frees the next.")
                              .arg(removed)
                              .arg(candidates_.size()));
    } else {
        summary_->setText(tr("Purged %1. Nothing else is unused.").arg(removed));
    }
}

}  // namespace cad::app
