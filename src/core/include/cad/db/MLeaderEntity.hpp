#pragma once

#include <array>
#include <string>
#include <vector>

#include "cad/db/Entity.hpp"

namespace cad {

/// A multileader — MULTILEADER, the entity other CAD software's MLEADER command makes.
///
/// One note with any number of arrows pointing at things. Until now it arrived
/// as an invisible placeholder: preserved perfectly on save and drawn as
/// nothing, which is the worst pair of behaviours available (ADR 0006). Any
/// drawing produced by other CAD software since 2008 is full of them.
///
/// **Read for its meaning, written back for its bytes**, on the same terms as a
/// table. Its content lives in a nested `CONTEXT_DATA` structure whose group
/// codes repeat at three depths and whose code 304 is *both* the note's text and
/// the marker that opens a leader line -- so nothing in it can be claimed by
/// code number, and there is no writer for it. What is parsed here is only what
/// is needed to draw it; every group goes back out exactly as it came in.
///
/// That has a consequence, and it is the reason `canTransform` is false: those
/// preserved bytes are what a save emits. Moving the entity without moving them
/// would write the multileader back where it started, silently, which is the
/// fault a table had until it was given the means to keep its bytes in step. A
/// multileader's points sit at three different nesting depths under repeated
/// codes, so the same trick does not reach them. Refusing the move, and saying
/// so, is the honest answer until it can be written properly.
class MLeaderEntity final : public Entity {
public:
    /// One arrow and the path back from it.
    struct LeaderRun {
        /// Vertices of the leader line, arrow point first.
        std::vector<Vec3> vertices;
        /// Where the note hangs: the far end of the dogleg.
        Vec3 landingEnd;
        bool hasLanding = false;
    };

    std::vector<LeaderRun> leaders;

    /// The note. Empty for a multileader whose content is a block.
    std::string text;
    Vec3 textLocation;
    double textHeight = 2.5;
    double textRotation = 0.0;
    /// Group 171 inside the context data: the nine-way MTEXT attachment.
    int attachment = 1;

    double arrowSize = 2.5;
    bool hasArrowheads = true;

    [[nodiscard]] std::string objectType() const override { return "MLeader"; }

    /// The leader lines and doglegs, as segments.
    [[nodiscard]] std::vector<std::pair<Vec3, Vec3>> segments() const;

    /// A filled triangle at each arrow point, pointing along its first segment.
    [[nodiscard]] std::vector<std::array<Vec3, 3>> arrowheads() const;

    [[nodiscard]] Box3 bounds() const override;
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    /// Deliberately does nothing. See the class comment: this entity is written
    /// from bytes that a transform cannot reach, and a move that appeared to
    /// work and then vanished on save is worse than one that was refused.
    void transformBy(const Mat4& m) override;

    [[nodiscard]] bool canTransform() const override { return false; }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
