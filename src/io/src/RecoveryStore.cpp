#include "cad/io/RecoveryStore.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace cad::io {
namespace {

/// Reduce a drawing name to something safe to put in a filename.
///
/// Recovery files are named after the drawing so the user can recognise them,
/// which means a drawing called `Section A/B` must not create a directory.
[[nodiscard]] std::string sanitize(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ' || c == '.';
        out.push_back(safe ? c : '_');
    }
    // Long names are truncated rather than rejected: a path limit must not be
    // the reason an auto-save silently stops happening.
    if (out.size() > 64) {
        out.resize(64);
    }
    if (out.empty()) {
        out = "drawing";
    }
    return out;
}

[[nodiscard]] std::string stemOf(std::string_view originalPath) {
    if (originalPath.empty()) {
        return "Unsaved";
    }
    return sanitize(std::filesystem::path{originalPath}.stem().string());
}

}  // namespace

std::string formatLocalTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    return buffer;
}

std::string RecoveryEntry::displayName() const {
    if (!originalPath.empty()) {
        return std::filesystem::path{originalPath}.filename().string();
    }
    return "Unsaved drawing";
}

RecoveryStore::RecoveryStore(std::filesystem::path directory, std::uint64_t processId)
    : directory_(std::move(directory)), processId_(processId) {}

std::filesystem::path RecoveryStore::pathFor(std::string_view originalPath) const {
    std::ostringstream name;
    name << stemOf(originalPath) << '_' << processId_ << kDrawingExtension;
    return directory_ / name.str();
}

std::filesystem::path RecoveryStore::metadataFor(const std::filesystem::path& drawing) const {
    std::filesystem::path metadata = drawing;
    metadata += ".txt";
    return metadata;
}

bool RecoveryStore::write(std::string_view originalPath, std::string_view drawingText) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    const std::filesystem::path target = pathFor(originalPath);
    std::filesystem::path temporary = target;
    temporary += ".part";

    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out) {
            return false;
        }
        out.write(drawingText.data(), static_cast<std::streamsize>(drawingText.size()));
        if (!out.good()) {
            out.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    // Replace only once the new file is complete. An interrupted auto-save then
    // costs the most recent interval rather than the whole recovery file.
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }

    std::ofstream meta{metadataFor(target), std::ios::binary | std::ios::trunc};
    if (!meta) {
        return false;
    }
    meta << "original=" << originalPath << '\n'
         << "saved=" << formatLocalTimestamp() << '\n'
         << "pid=" << processId_ << '\n';
    return meta.good();
}

bool RecoveryStore::discard(std::string_view originalPath) {
    const std::filesystem::path target = pathFor(originalPath);
    std::error_code ec;
    const bool removed = std::filesystem::remove(target, ec);
    std::filesystem::remove(metadataFor(target), ec);
    return removed;
}

void RecoveryStore::discardAll() {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory_, ec)) {
        return;
    }
    const std::string suffix = "_" + std::to_string(processId_) + kDrawingExtension;
    for (const auto& item : std::filesystem::directory_iterator{directory_, ec}) {
        const std::string name = item.path().filename().string();
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            std::filesystem::remove(item.path(), ec);
            std::filesystem::remove(metadataFor(item.path()), ec);
        }
    }
}

std::vector<RecoveryEntry> RecoveryStore::pending(
    const std::function<bool(std::uint64_t)>& isProcessAlive) const {
    std::vector<RecoveryEntry> entries;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory_, ec)) {
        return entries;
    }

    for (const auto& item : std::filesystem::directory_iterator{directory_, ec}) {
        // Only the drawings. The metadata sidecar ends `.txt` and a partial
        // write ends `.part`, so both are skipped by the same test — which is
        // what keeps interrupted debris from being offered as a rescue.
        if (!item.is_regular_file(ec) || item.path().extension() != kDrawingExtension) {
            continue;
        }

        RecoveryEntry entry;
        entry.recoveryFile = item.path();

        std::ifstream meta{metadataFor(item.path())};
        std::string line;
        while (std::getline(meta, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, equals);
            const std::string value = line.substr(equals + 1);
            if (key == "original") {
                entry.originalPath = value;
            } else if (key == "saved") {
                entry.savedAt = value;
            } else if (key == "pid") {
                try {
                    entry.processId = std::stoull(value);
                } catch (const std::exception&) {
                    entry.processId = 0;
                }
            }
        }

        // A session still running owns its files. Offering them would invite
        // the user to "recover" a drawing that is open and being edited in
        // another window.
        if (entry.processId != 0 && isProcessAlive && isProcessAlive(entry.processId)) {
            continue;
        }
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(),
              [](const RecoveryEntry& a, const RecoveryEntry& b) { return a.savedAt > b.savedAt; });
    return entries;
}

bool RecoveryStore::remove(const RecoveryEntry& entry) {
    std::error_code ec;
    std::filesystem::path metadata = entry.recoveryFile;
    metadata += ".txt";
    const bool removed = std::filesystem::remove(entry.recoveryFile, ec);
    std::filesystem::remove(metadata, ec);
    return removed;
}

}  // namespace cad::io
