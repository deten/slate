#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cad::io {

/// One drawing recovered from an interrupted session.
struct RecoveryEntry {
    /// The auto-saved drawing itself.
    std::filesystem::path recoveryFile;
    /// Where the drawing came from, empty when it was never saved.
    std::string originalPath;
    /// When it was written, as `YYYY-MM-DD HH:MM:SS` local time — for showing
    /// the user how much work the file represents before they commit to it.
    std::string savedAt;
    /// The session that wrote it, so a live instance's files are not offered
    /// back to a second instance as though that session had crashed.
    std::uint64_t processId = 0;

    /// What to call this drawing when asking the user about it.
    [[nodiscard]] std::string displayName() const;
};

/// Auto-save files and the crash recovery that reads them back.
///
/// The design constraint is that this must never make things worse. It writes
/// *beside* the user's work rather than over it, so a crash mid-auto-save
/// cannot damage the file being protected, and a recovery file is only ever
/// discarded once the real drawing has been saved or the user has declined it.
///
/// Files are named after the drawing and the writing process, which is what
/// lets a second instance tell "this session crashed" from "another window has
/// this open right now".
class RecoveryStore {
public:
    /// @param directory where recovery files live. Created on first write.
    /// @param processId this session's identifier.
    RecoveryStore(std::filesystem::path directory, std::uint64_t processId);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

    /// Where this session's auto-save for `originalPath` goes.
    /// An unsaved drawing gets a name derived from the session alone.
    [[nodiscard]] std::filesystem::path pathFor(std::string_view originalPath) const;

    /// Write an auto-save and its metadata.
    ///
    /// The drawing is written to a temporary name and moved into place, so an
    /// interrupted write leaves the *previous* recovery file intact rather than
    /// a half-written one. A truncated recovery file is worse than none: it
    /// looks like a rescue and is not.
    ///
    /// @returns false when the file could not be written.
    bool write(std::string_view originalPath, std::string_view drawingText);

    /// Remove this session's auto-save for a drawing. Called once the real file
    /// has been saved, and on a clean exit — a recovery file that outlives the
    /// work it was protecting is an offer to restore something already saved.
    bool discard(std::string_view originalPath);

    /// Remove every file this session wrote.
    void discardAll();

    /// Auto-saves left behind by sessions that are no longer running.
    ///
    /// @param isProcessAlive decides whether a session still holds its files.
    ///   Injected because asking the operating system about a process is not
    ///   something the engine should know how to do, and because a test needs
    ///   to answer it without starting one. Returning true for everything makes
    ///   this report nothing, which is the safe direction to fail in.
    [[nodiscard]] std::vector<RecoveryEntry> pending(
        const std::function<bool(std::uint64_t)>& isProcessAlive) const;

    /// Delete a recovery file and its metadata, after the user has taken or
    /// declined it.
    static bool remove(const RecoveryEntry& entry);

    /// Extension of the auto-saved drawing. DXF rather than DWG because it is
    /// what this application writes losslessly and without a converter — a
    /// recovery path that depends on an external tool is one that can fail at
    /// exactly the wrong moment.
    static constexpr const char* kDrawingExtension = ".sv$";
    static constexpr const char* kMetadataExtension = ".sv$.txt";

private:
    [[nodiscard]] std::filesystem::path metadataFor(const std::filesystem::path& drawing) const;

    std::filesystem::path directory_;
    std::uint64_t processId_ = 0;
};

/// A local timestamp as `YYYY-MM-DD HH:MM:SS`.
[[nodiscard]] std::string formatLocalTimestamp();

}  // namespace cad::io
