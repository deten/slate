#include "cad/io/DxfRecord.hpp"

#include <algorithm>

namespace cad::io {
namespace {

/// Convert a DXF group value to the XData variant the database stores.
[[nodiscard]] XDataValue toXDataValue(const DxfGroup& group) {
    if (const auto* text = std::get_if<std::string>(&group.value)) {
        return *text;
    }
    if (const auto* number = std::get_if<double>(&group.value)) {
        return *number;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&group.value)) {
        return *integer;
    }
    return group.asString();
}

}  // namespace

DxfRecord::DxfRecord(std::string type, std::vector<DxfGroup> groups)
    : type_(std::move(type)), groups_(std::move(groups)) {
    std::string currentXDataApp;

    // Group 101 hands the rest of the record to an embedded object whose codes
    // start again from scratch. Indexing past it would mix two objects'
    // meanings under one code.
    const auto marker = std::find_if(groups_.begin(), groups_.end(),
                                     [](const DxfGroup& group) { return group.code == 101; });
    ownCount_ = static_cast<std::size_t>(std::distance(groups_.begin(), marker));

    for (std::size_t i = 0; i < groups_.size(); ++i) {
        const DxfGroup& group = groups_[i];

        // XData is scanned across the whole record, not just this record's own
        // part: it is written *after* any embedded object, so stopping at the
        // marker would silently drop every third-party application's data on
        // exactly the entities that have an embedded object.
        //
        // Code 1001 opens an XData block belonging to a named application;
        // every XData code after it belongs to that app until the next 1001.
        if (group.code == 1001) {
            currentXDataApp = group.asString();
            xdata_.try_emplace(currentXDataApp);
            continue;
        }
        if (isXDataCode(group.code)) {
            if (!currentXDataApp.empty()) {
                xdata_[currentXDataApp].push_back({group.code, toXDataValue(group)});
            }
            continue;
        }

        if (i < ownCount_) {
            byCode_[group.code].push_back(i);
        } else {
            embedded_.push_back({group.code, toXDataValue(group)});
        }
    }
}

const DxfGroup* DxfRecord::first(int code) const {
    consumed_.push_back(code);
    const auto it = byCode_.find(code);
    if (it == byCode_.end() || it->second.empty()) {
        return nullptr;
    }
    return &groups_[it->second.front()];
}

std::vector<std::string> DxfRecord::allStrings(int code) const {
    consumed_.push_back(code);
    std::vector<std::string> values;
    const auto it = byCode_.find(code);
    if (it == byCode_.end()) {
        return values;
    }
    values.reserve(it->second.size());
    for (const std::size_t index : it->second) {
        values.push_back(groups_[index].asString());
    }
    return values;
}

std::string DxfRecord::text(int code, std::string fallback) const {
    const DxfGroup* group = first(code);
    return group == nullptr ? std::move(fallback) : group->asString();
}

std::optional<std::string> DxfRecord::optionalText(int code) const {
    const DxfGroup* group = first(code);
    return group == nullptr ? std::nullopt : std::optional{group->asString()};
}

double DxfRecord::number(int code, double fallback) const {
    const DxfGroup* group = first(code);
    return group == nullptr ? fallback : group->asDouble();
}

std::optional<double> DxfRecord::optionalNumber(int code) const {
    const DxfGroup* group = first(code);
    return group == nullptr ? std::nullopt : std::optional{group->asDouble()};
}

std::int64_t DxfRecord::integer(int code, std::int64_t fallback) const {
    const DxfGroup* group = first(code);
    return group == nullptr ? fallback : group->asInteger();
}

std::optional<std::int64_t> DxfRecord::optionalInteger(int code) const {
    const DxfGroup* group = first(code);
    return group == nullptr ? std::nullopt : std::optional{group->asInteger()};
}

bool DxfRecord::boolean(int code, bool fallback) const {
    const DxfGroup* group = first(code);
    return group == nullptr ? fallback : group->asBool();
}

Vec3 DxfRecord::point(int xCode, const Vec3& fallback) const {
    if (!byCode_.contains(xCode)) {
        consumed_.push_back(xCode);
        return fallback;
    }
    return Vec3{number(xCode, fallback.x), number(xCode + 10, fallback.y),
                number(xCode + 20, fallback.z)};
}

std::optional<Vec3> DxfRecord::optionalPoint(int xCode) const {
    if (!byCode_.contains(xCode)) {
        return std::nullopt;
    }
    return point(xCode);
}

Vec3 DxfRecord::extrusion() const {
    const Vec3 normal = point(210, Vec3::unitZ());
    return normal.tryNormalize().value_or(Vec3::unitZ());
}

std::optional<std::string> DxfRecord::ownerHandleText() const {
    consumed_.push_back(330);

    // A `102` whose value opens with '{' begins an application group and a bare
    // `102 }` ends it. Nesting is not legal in DXF, but a counter costs nothing
    // and degrades more gracefully than a boolean on a malformed file.
    int depth = 0;
    for (const DxfGroup& group : groups()) {
        if (group.code == 102) {
            const std::string value = group.asString();
            if (!value.empty() && value.front() == '{') {
                ++depth;
            } else if (depth > 0) {
                --depth;
            }
            continue;
        }
        if (group.code == 330 && depth == 0) {
            return group.asString();
        }
    }
    return std::nullopt;
}

void DxfRecord::markConsumed(std::initializer_list<int> codes) const {
    consumed_.insert(consumed_.end(), codes.begin(), codes.end());
}

std::vector<UnknownGroup> DxfRecord::unconsumed() const {
    std::vector<UnknownGroup> result;

    // File order, not code order.
    //
    // A DICTIONARY is a run of alternating pairs — `3` names an entry and the
    // `350` after it is the object. Emitting every 3 and then every 350 keeps
    // both lists complete and pairs each name with the wrong object, which is
    // how a saved drawing loses its layer states, its groups, its field
    // definitions and its plot settings while appearing to contain them all.
    //
    // Nothing wanted code order. It was simply what iterating the index gave.
    // Codes 5 and 330 are structural *once*, and data every other time.
    //
    // A record's own handle is its first code 5 and its owner is its first
    // code 330, and the writer re-derives both — so echoing them back would
    // duplicate them. Every later occurrence means something else entirely, and
    // skipping the code rather than the occurrence deleted all of it. A
    // SORTENTSTABLE is the case that shows how much: it is a run of 331/5 pairs
    // saying which entity draws on top of which, and it came back out of a save
    // with all 1,749 of its code-5 halves gone. Draw order is not decoration —
    // it is what puts a solid fill under the text it would otherwise hide.
    //
    // Occurrences inside a `102 {...}` application group are never structural.
    // The 330s in `{ACAD_REACTORS}` are the reactor list, which is how other CAD software
    // knows an image definition or a group still has users; emptying it invites
    // the next PURGE to delete things that are still in the drawing.
    bool skippedHandle = false;
    bool skippedOwner = false;
    int applicationDepth = 0;

    for (std::size_t i = 0; i < ownCount_; ++i) {
        const DxfGroup& group = groups_[i];
        if (group.code == 1001 || isXDataCode(group.code)) {
            continue;  // Kept separately, and written after everything else.
        }
        if (group.code == 102) {
            // `102 {NAME` opens a group and `102 }` closes it. Nesting does not
            // occur in practice, but counting costs nothing and a stray closer
            // cannot then take the rest of the record out of the group.
            const std::string value = group.asString();
            if (value.starts_with("{")) {
                ++applicationDepth;
            } else if (applicationDepth > 0) {
                --applicationDepth;
            }
            result.push_back({group.code, toXDataValue(group)});
            continue;
        }

        if (applicationDepth == 0) {
            if (group.code == 5 && !skippedHandle) {
                skippedHandle = true;
                continue;
            }
            if (group.code == 330 && !skippedOwner) {
                skippedOwner = true;
                continue;
            }
        }

        if (group.code == 5 || group.code == 330) {
            // Past the structural one, so it is data and the consumed list —
            // which records codes, not occurrences — must not veto it.
            result.push_back({group.code, toXDataValue(group)});
            continue;
        }

        if (std::find(consumed_.begin(), consumed_.end(), group.code) != consumed_.end()) {
            continue;
        }
        // The record type marker is re-derived on write and would be duplicated.
        //
        // Code 100 is *not* in that list. It names the subclass a record's
        // following groups belong to, and only a writer that understands the
        // record can re-derive it — which for a preserved record is exactly what
        // we are not. Callers writing their own markers drop these; callers
        // echoing the record back need them, or a DICTIONARY comes out with no
        // subclass marker at all.
        if (group.code == 0) {
            continue;
        }
        result.push_back({group.code, toXDataValue(group)});
    }
    return result;
}

}  // namespace cad::io
