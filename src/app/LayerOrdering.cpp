#include "LayerOrdering.hpp"

#include <algorithm>

#include <QCollator>
#include <QString>

#include "cad/db/Database.hpp"
#include "cad/db/Layer.hpp"

namespace cad::app {

std::vector<Handle> layersByName(const Database& db) {
    std::vector<Handle> handles;
    for (const Handle handle : db.layers().handles()) {
        const auto* layer = db.get<Layer>(handle);
        if (layer != nullptr && !layer->erased) {
            handles.push_back(handle);
        }
    }

    // Built once rather than per comparison: a collator carries locale data and
    // constructing one inside the predicate would rebuild it for every pair.
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    std::sort(handles.begin(), handles.end(), [&](Handle a, Handle b) {
        const auto* first = db.get<Layer>(a);
        const auto* second = db.get<Layer>(b);
        if (first == nullptr || second == nullptr) {
            return first != nullptr;
        }
        const QString left = QString::fromStdString(first->name);
        const QString right = QString::fromStdString(second->name);
        const int order = collator.compare(left, right);
        if (order != 0) {
            return order < 0;
        }
        // Two layers whose names differ only in case: ordered by the exact
        // name, so the list is stable rather than depending on which the
        // sort happened to see first.
        return first->name < second->name;
    });
    return handles;
}

}  // namespace cad::app
