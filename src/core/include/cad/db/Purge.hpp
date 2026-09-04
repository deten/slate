#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cad/db/Handle.hpp"

namespace cad {

class Database;
class Transaction;

/// A named object nothing in the drawing refers to any more.
struct PurgeCandidate {
    enum class Kind : std::uint8_t {
        Block,
        Layer,
        Linetype,
        TextStyle,
        DimStyle,
    };

    Kind kind = Kind::Block;
    Handle handle;
    std::string name;
    /// How many objects the record still holds, for a block. Shown because
    /// "Binder2 (1 image)" is a very different proposition from an empty block
    /// left behind by a delete, and the user is the one who knows which.
    std::size_t contents = 0;
};

/// Whether a block record's name was invented by the application rather than
/// chosen by a person.
///
/// Two families qualify. Names beginning `*` are other CAD software's own anonymous
/// records -- the geometry behind a dimension (`*D4`), a table (`*T19`), a
/// hatch or an unnamed group. Names of the form `A$C` followed by hex are the
/// copies it makes when a block is pasted or redefined.
///
/// Worth telling apart because a user can decide about "Binder2" and cannot
/// decide about "A$Cc31f4d03": one is a drawing they recognise and the other is
/// debris with a serial number. A list that mixes them buries the decision that
/// matters among forty that do not.
[[nodiscard]] bool isMachineNamedBlock(std::string_view name) noexcept;

/// The word for one kind, for a list a user reads.
[[nodiscard]] std::string_view purgeKindName(PurgeCandidate::Kind kind) noexcept;

/// Which kinds a purge is allowed to take.
///
/// Applied inside the analysis rather than to its result, and that is the whole
/// reason it exists as a parameter instead of a filter the caller applies
/// afterwards. A layer is unused *because* the block whose geometry sat on it
/// is going; keep the blocks and that layer is still in use. Filtering the
/// finished list would offer to delete a layer that is about to have something
/// on it again.
enum class PurgeKinds : std::uint8_t {
    None = 0,
    Blocks = 1U << 0U,
    Layers = 1U << 1U,
    Linetypes = 1U << 2U,
    TextStyles = 1U << 3U,
    DimStyles = 1U << 4U,
    Styles = TextStyles | DimStyles,
    All = Blocks | Layers | Linetypes | TextStyles | DimStyles,
};

[[nodiscard]] constexpr PurgeKinds operator|(PurgeKinds a, PurgeKinds b) noexcept {
    return static_cast<PurgeKinds>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr bool contains(PurgeKinds set, PurgeKinds one) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(one)) != 0;
}

/// Everything unused, found by repeated sweeps.
///
/// Sweeps repeat because purging cascades: a block nothing inserts is unused,
/// and the layer only its contents were drawn on becomes unused the moment that
/// block goes. One pass would report the block and leave the layer behind for
/// the user to run the command again to find, which is how a purge that claims
/// to be finished is not.
///
/// What is never offered, whatever its reference count:
///
/// - Layer `0`, linetype `Continuous`, `ByLayer` and `ByBlock`, text style
///   `Standard`, dimension style `Standard`. A drawing without these is
///   malformed, and every reader has to invent them again on load.
/// - The current layer, text style and dimension style, which are about to be
///   used by the next thing drawn.
/// - Model space and every paper space, which are layouts rather than blocks.
/// - Xref block records. An unloaded xref refers to nothing in this drawing and
///   would look purgeable, but discarding it is *detaching a reference*, not
///   tidying up, and the two should not share a command.
[[nodiscard]] std::vector<PurgeCandidate> findPurgeable(const Database& db,
                                                       PurgeKinds allowed = PurgeKinds::All);

/// Erase `candidates`, through `transaction` so the whole purge undoes as one.
///
/// Returns how many were erased. Candidates that have gone or turned out to be
/// referenced since `findPurgeable` ran are skipped rather than failing the
/// purge, because a stale list is a normal consequence of showing it to a user
/// and waiting for an answer.
std::size_t purge(Transaction& transaction, const std::vector<PurgeCandidate>& candidates);

}  // namespace cad
