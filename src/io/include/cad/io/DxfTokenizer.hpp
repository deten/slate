#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cad::io {

/// The data type a DXF group code's value carries.
enum class GroupValueType : std::uint8_t {
    Text,
    Integer,
    Double,
    Boolean,
    /// A hexadecimal object handle. Must not be number-parsed as a double.
    Handle,
    /// Binary chunk, hex-encoded in ASCII DXF.
    Binary,
};

/// Determine a group code's value type from its numeric range.
///
/// The ranges come from the DXF reference. Getting this wrong produces subtly
/// corrupt data rather than a parse error — a coordinate read as a string, or
/// an integer truncated from a double — so they are enumerated explicitly
/// rather than approximated.
[[nodiscard]] GroupValueType groupValueType(int code) noexcept;

/// True when the code carries application-defined extended data.
[[nodiscard]] constexpr bool isXDataCode(int code) noexcept {
    return code >= 1000 && code <= 1071;
}

/// True for the X component of a point triple.
///
/// DXF encodes points as three consecutive codes: `n`, `n + 10`, `n + 20` for
/// X, Y and Z. Recognizing the X code is what lets the reader reassemble them
/// into a point rather than tracking three loose numbers.
[[nodiscard]] constexpr bool isPointXCode(int code) noexcept {
    return (code >= 10 && code <= 18) || (code >= 110 && code <= 112) ||
           (code >= 210 && code <= 219);
}

/// A value read from a DXF stream, in its typed form.
using GroupValue = std::variant<std::string, double, std::int64_t, bool>;

/// One (code, value) pair.
struct DxfGroup {
    int code = 0;
    GroupValue value;
    /// Source line number, so diagnostics point at the real file.
    std::size_t line = 0;

    [[nodiscard]] std::string asString() const;
    [[nodiscard]] double asDouble() const noexcept;
    [[nodiscard]] std::int64_t asInteger() const noexcept;
    [[nodiscard]] bool asBool() const noexcept;
};

/// A problem encountered while reading, reported rather than thrown.
struct DxfDiagnostic {
    enum class Severity : std::uint8_t { Info, Warning, Error };

    Severity severity = Severity::Warning;
    std::string message;
    std::size_t line = 0;
};

/// Tokenizer for ASCII DXF.
///
/// DXF is line-oriented: an integer group code on one line, its value on the
/// next. That looks trivial, and the naive split-on-newline implementation is
/// exactly why so many DXF readers mangle real files. This one handles what
/// real files actually contain:
///
///  - CRLF, LF and lone-CR line endings, sometimes mixed within one file;
///  - leading and trailing whitespace around codes, which other CAD software pads;
///  - a UTF-8 byte order mark;
///  - malformed pairs, which are reported and skipped rather than aborting the
///    load — a professional drawing with one bad group is still worth opening.
class DxfTokenizer {
public:
    explicit DxfTokenizer(std::string_view text);

    /// Read the next group, or empty at end of input.
    [[nodiscard]] std::optional<DxfGroup> next();

    [[nodiscard]] const std::vector<DxfDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

    /// Read every remaining group. Convenience for tests and small files.
    [[nodiscard]] std::vector<DxfGroup> readAll();

private:
    /// Read one raw line, advancing past whichever terminator follows.
    [[nodiscard]] std::optional<std::string_view> readLine();

    [[nodiscard]] std::optional<GroupValue> parseValue(int code, std::string_view raw);

    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t lineNumber_ = 1;
    std::vector<DxfDiagnostic> diagnostics_;
};

/// A cursor over a group stream with the one-item lookahead a recursive-descent
/// parser needs.
///
/// DXF parsing is fundamentally "consume groups until a code that starts the
/// next thing", which requires peeking without consuming. Buffering the whole
/// stream would defeat the streaming tokenizer, so this keeps a single pushback
/// slot instead.
class GroupCursor {
public:
    explicit GroupCursor(DxfTokenizer& tokenizer) : tokenizer_(tokenizer) {}

    [[nodiscard]] std::optional<DxfGroup> next();
    [[nodiscard]] const DxfGroup* peek();

    /// Return a group to the stream so the next `next()` yields it again.
    void pushBack(DxfGroup group);

    [[nodiscard]] bool atEnd() { return peek() == nullptr; }

private:
    DxfTokenizer& tokenizer_;
    std::optional<DxfGroup> pushedBack_;
};

/// Common DXF section names.
namespace section {
inline constexpr std::string_view kHeader = "HEADER";
inline constexpr std::string_view kClasses = "CLASSES";
inline constexpr std::string_view kTables = "TABLES";
inline constexpr std::string_view kBlocks = "BLOCKS";
inline constexpr std::string_view kEntities = "ENTITIES";
inline constexpr std::string_view kObjects = "OBJECTS";
}  // namespace section

/// Human-readable release name for a `$ACADVER` version code.
[[nodiscard]] std::string_view dxfVersionName(std::string_view code) noexcept;

}  // namespace cad::io
