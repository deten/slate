#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cad {

/// A user-editable configuration file, read and written without losing anything.
///
/// The format is deliberately the plainest thing that works — sections in
/// brackets, `key = value` beneath them, `#` or `;` starting a whole-line
/// comment:
///
/// ```ini
/// # My aliases
/// [aliases]
/// c = COPY      ; overriding the built-in C for CIRCLE
/// ```
///
/// The reason it is not JSON is that a person is expected to open it. The brief
/// requires users to customise aliases, shortcuts and defaults, and a format
/// that punishes a trailing comma is hostile to that.
///
/// **Every original line is preserved.** Writing a setting rewrites that one
/// line and leaves comments, blank lines, ordering and unknown keys exactly as
/// they were. A configuration file that silently reformats itself — or drops the
/// comments explaining why a setting is what it is — teaches users not to edit
/// it, and then not to trust it.
///
/// Comments are whole-line only. A `#` after a value is part of the value, so
/// that no value needs escaping and nothing is ambiguous.
class ConfigFile {
public:
    ConfigFile() = default;

    /// Parse configuration text. Malformed lines are kept verbatim and ignored
    /// rather than rejected: one bad line must not cost the user the rest of
    /// their settings.
    [[nodiscard]] static ConfigFile parse(std::string_view text);

    /// Read from disk. A missing file yields an empty configuration rather than
    /// an error — no settings is a valid state, and the common one on first run.
    [[nodiscard]] static ConfigFile load(const std::filesystem::path& path);

    /// Serialize back to text, preserving the original layout.
    [[nodiscard]] std::string toText() const;

    /// Write to disk, creating parent directories as needed.
    /// @returns false when the file could not be written.
    bool save(const std::filesystem::path& path) const;

    [[nodiscard]] bool has(std::string_view section, std::string_view key) const;

    /// Raw value, or nothing when absent.
    [[nodiscard]] std::optional<std::string> get(std::string_view section,
                                                 std::string_view key) const;

    [[nodiscard]] std::string text(std::string_view section, std::string_view key,
                                   std::string fallback = {}) const;
    [[nodiscard]] double number(std::string_view section, std::string_view key,
                                double fallback = 0.0) const;
    [[nodiscard]] long long integer(std::string_view section, std::string_view key,
                                    long long fallback = 0) const;

    /// Booleans accept the spellings a person actually types: true/false,
    /// yes/no, on/off, 1/0.
    [[nodiscard]] bool boolean(std::string_view section, std::string_view key,
                               bool fallback = false) const;

    /// Set a value, updating the existing line in place when the key is already
    /// present so surrounding comments and ordering survive.
    void set(std::string_view section, std::string_view key, std::string_view value);

    void setNumber(std::string_view section, std::string_view key, double value);
    void setBoolean(std::string_view section, std::string_view key, bool value);

    /// Remove a key. @returns true when it was there.
    bool remove(std::string_view section, std::string_view key);

    /// Every key and value in a section, in file order.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> entries(
        std::string_view section) const;

    /// Section names, in file order.
    [[nodiscard]] std::vector<std::string> sections() const;

    [[nodiscard]] bool empty() const noexcept { return lines_.empty(); }

private:
    /// One physical line, kept whether or not it means anything to us.
    struct Line {
        enum class Kind : std::uint8_t { Other, Section, Entry };

        Kind kind = Kind::Other;
        /// Verbatim text, used for anything we do not rewrite.
        std::string raw;
        /// Normalized section this line belongs to (entries) or names (headers).
        std::string section;
        /// Normalized key, for lookup.
        std::string key;
        /// Key as the user spelled it, so rewriting preserves their casing.
        std::string keyAsWritten;
        std::string value;
        /// Leading whitespace, preserved when the line is rewritten.
        std::string indent;
        /// True once `set` has changed this line's value.
        ///
        /// Untouched lines are written back byte for byte, so a file the user
        /// spaced out as `zz   =   ZOOM` is not quietly restyled by an edit to
        /// some unrelated key. Only what actually changed is reformatted.
        bool edited = false;
    };

    [[nodiscard]] const Line* find(std::string_view section, std::string_view key) const;
    [[nodiscard]] Line* find(std::string_view section, std::string_view key);

    /// Index just past the last line of a section, where a new key is appended.
    /// Trailing blank lines are skipped so additions land against the content
    /// rather than after the gap separating it from the next section.
    [[nodiscard]] std::size_t appendPositionFor(std::string_view section) const;

    std::vector<Line> lines_;
};

/// Lower-case a name for case-insensitive comparison, as commands and sections
/// both need.
[[nodiscard]] std::string normalizeConfigName(std::string_view name);

}  // namespace cad
