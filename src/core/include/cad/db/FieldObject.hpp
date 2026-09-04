#pragma once

#include <string>
#include <vector>

#include "cad/db/DbObject.hpp"

namespace cad {

/// A field: text whose value is *derived* rather than typed.
///
/// A room label reading "MASTER BEDROOM 423.08" is not three pieces of text on a
/// drawing. It is the name of a space, the area of the polyline that bounds it,
/// and a format — and when the polyline is stretched, the number follows. That
/// is what a field is for, and it is why the number in a professional drawing
/// can be trusted at all.
///
/// Structure is a small tree. A `_text` field holds literal text with holes in
/// it, and each hole names a child field by index; the children are the ones
/// that actually go and look something up. The sample drawing's labels are
/// exactly that: `2nd %<\_FldIdx 0>%` over an `AcObjProp` child that reads
/// `TextString` from another entity.
///
/// The evaluated result is *cached* in the file, and the cache is what an
/// application that cannot evaluate fields displays. Keeping it matters: it is
/// the only value available for a field naming something we do not implement,
/// and showing a stale number beats showing an error.
class FieldObject final : public DbObject {
public:
    /// Which evaluator computes this field, from group 1.
    ///
    /// `_text` is literal text with child holes; `AcObjProp` reads a property of
    /// another object; `AcExpr` is arithmetic over children; `AcVar` is a
    /// drawing or system variable. A version suffix is common and carries no
    /// meaning here — `AcObjProp.16.2` evaluates as `AcObjProp`.
    std::string evaluatorId;

    /// The field code, from group 2: the expression as the user's application
    /// wrote it, e.g. `\AcObjProp Object(%<\_ObjIdx 0>%).Area \f "%lu2%pr2"`.
    std::string fieldCode;

    /// The last value the authoring application computed, from group 301.
    ///
    /// Shown when the field cannot be evaluated here. It is not authoritative:
    /// a field is stale by definition the moment the thing it names changes.
    std::string cachedValue;

    /// The format the authoring application actually applied, from the value
    /// block rather than from the field code.
    ///
    /// The two differ, and only this one is complete. A schedule total's code
    /// says ` "%lu2"` -- a unit style and no precision -- while its value
    /// block says `%pr2%lu2`, and it is the `%pr2` that decides the total reads
    /// 1142.30 rather than the drawing's `LUPREC` of six decimals. Without it a
    /// correct number is displayed wrongly, which on a schedule is
    /// indistinguishable from a wrong number.
    std::string appliedFormat;

    /// Child fields, in the order `%<\_FldIdx N>%` indexes them.
    std::vector<Handle> childFields;

    /// Objects the code refers to, in the order `%<\_ObjIdx N>%` indexes them.
    std::vector<Handle> referencedObjects;

    [[nodiscard]] std::string objectType() const override { return "Field"; }

    /// The evaluator with any version suffix removed.
    [[nodiscard]] std::string evaluatorName() const;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

}  // namespace cad
