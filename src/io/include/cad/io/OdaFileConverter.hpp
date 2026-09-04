#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cad/io/XrefLoader.hpp"

namespace cad::io {

/// Why a translation failed.
///
/// Distinguished because the right response differs: a missing converter is
/// something the user can install, a corrupt file is not, and a timeout means
/// try again rather than give up.
enum class TranslationFailure : std::uint8_t {
    TranslatorUnavailable,
    SourceUnreadable,
    TranslatorFailed,
    UnsupportedFormat,
    Timeout,
};

/// A translation failure carrying something the user can act on.
class TranslationError : public std::runtime_error {
public:
    TranslationError(TranslationFailure failure, std::string message, std::string remedy = {})
        : std::runtime_error(message.empty() ? "DWG translation failed" : message),
          failure_(failure),
          remedy_(std::move(remedy)) {}

    [[nodiscard]] TranslationFailure failure() const noexcept { return failure_; }

    /// What the user should do about it. Shown verbatim, so it is written for
    /// a drafter rather than for a developer.
    [[nodiscard]] const std::string& remedy() const noexcept { return remedy_; }

private:
    TranslationFailure failure_;
    std::string remedy_;
};

/// DWG translation through the ODA File Converter.
///
/// The converter is a free, no-account command-line tool from the Open Design
/// Alliance (see ADR 0001). Its interface is *directory*-oriented, not
/// file-oriented:
///
/// ```
///   ODAFileConverter <inDir> <outDir> <outVer> <outType> <recurse> <audit> [filter]
/// ```
///
/// That shape drives the whole implementation. Every conversion runs in a
/// private temporary directory holding exactly one staged copy of the file, so
/// that opening one drawing never converts the two hundred others sitting in
/// the same folder as a side effect.
///
/// Two behaviours of the tool are worth knowing before reading the code, since
/// both look like bugs here otherwise:
///
///  - **It exits 0 having written nothing** for several classes of failure. So
///    success is decided by an output file appearing, never by the exit code.
///  - **It is a Qt GUI application run headless.** On a machine without a
///    usable display it can block rather than exit, which is why the timeout is
///    mandatory rather than a configurable nicety.
class OdaFileConverter final : public DwgTranslator {
public:
    /// @param explicitPath overrides the search entirely, for a converter
    ///   installed somewhere unusual or shipped alongside the application.
    explicit OdaFileConverter(std::filesystem::path explicitPath = {});

    [[nodiscard]] std::string id() const override { return "oda-file-converter"; }
    [[nodiscard]] std::string displayName() const override { return "ODA File Converter"; }

    [[nodiscard]] Status probe() const override;

    /// @throws TranslationError, always carrying a remedy.
    [[nodiscard]] std::string toDxf(const std::filesystem::path& source) const override;

    /// Write DXF text out as a DWG.
    ///
    /// Not part of `DwgTranslator`, because reading and writing are separable:
    /// a future direct-read binding might not write, and requiring it to would
    /// keep a perfectly good reader out.
    void fromDxf(const std::string& dxfText, const std::filesystem::path& target,
                 std::string_view version = "ACAD2018") const;

    [[nodiscard]] bool canWriteDwg() const noexcept override { return true; }

    /// Writes at the same version the reader is pinned to, so a drawing saved
    /// here opens in the same releases the one it came from does.
    void writeDwg(const std::string& dxfText,
                  const std::filesystem::path& target) const override {
        fromDxf(dxfText, target);
    }

    void setPath(std::filesystem::path path) { explicitPath_ = std::move(path); }

    /// Bound on one conversion. Generous by default: a large drawing genuinely
    /// takes tens of seconds, and killing a working conversion is worse than
    /// waiting.
    void setTimeout(std::chrono::milliseconds timeout) { timeout_ = timeout; }

    void setProgressCallback(std::function<void(const std::string&)> callback) {
        onProgress_ = std::move(callback);
    }

    /// The located executable, or an empty path when none was found.
    [[nodiscard]] std::filesystem::path findExecutable() const;

    /// Everywhere the converter is looked for, in priority order.
    ///
    /// Exposed so the UI can tell the user exactly where it looked, which is
    /// far more useful than "not found".
    [[nodiscard]] static std::vector<std::filesystem::path> searchLocations();

    /// Environment variable holding an explicit converter path. Checked first,
    /// so a test or a CI run can point at a converter without installing one.
    static constexpr const char* kPathEnvironmentVariable = "CAD_ODA_CONVERTER";

private:
    std::filesystem::path explicitPath_;
    std::chrono::milliseconds timeout_{180'000};
    std::function<void(const std::string&)> onProgress_;
};

/// The DWG translator this build should use.
///
/// A function rather than a registry: there is one translator, and inventing a
/// selection mechanism before there is anything to select between would be
/// architecture for its own sake. When a second one exists — a direct ODA SDK
/// binding — this becomes the place that chooses.
[[nodiscard]] std::unique_ptr<DwgTranslator> makeDwgTranslator();

}  // namespace cad::io
