#include "cad/io/DxfTokenizer.hpp"

#include <charconv>
#include <cctype>
#include <stdexcept>

namespace cad::io {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto isSpace = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

GroupValueType groupValueType(int code) noexcept {
    // Handles are hex strings and must never be number-parsed: "1E" would
    // otherwise become 1 or a float.
    if (code == 5 || code == 105) return GroupValueType::Handle;
    if (code >= 320 && code <= 369) return GroupValueType::Handle;
    if (code >= 390 && code <= 399) return GroupValueType::Handle;
    if (code >= 480 && code <= 481) return GroupValueType::Handle;
    if (code == 1005) return GroupValueType::Handle;

    if (code >= 0 && code <= 9) return GroupValueType::Text;
    // 10-59: doubles (coordinates, distances, angles, scale factors).
    if (code >= 10 && code <= 59) return GroupValueType::Double;
    // 60-79: 16-bit integers.
    if (code >= 60 && code <= 79) return GroupValueType::Integer;
    // 90-99: 32-bit integers.
    if (code >= 90 && code <= 99) return GroupValueType::Integer;
    // 100 subclass marker, 101 embedded-object marker, 102 grouping.
    if (code >= 100 && code <= 102) return GroupValueType::Text;
    if (code >= 110 && code <= 149) return GroupValueType::Double;
    if (code >= 160 && code <= 179) return GroupValueType::Integer;
    if (code >= 210 && code <= 239) return GroupValueType::Double;
    if (code >= 270 && code <= 289) return GroupValueType::Integer;
    if (code >= 290 && code <= 299) return GroupValueType::Boolean;
    if (code >= 300 && code <= 309) return GroupValueType::Text;
    if (code >= 310 && code <= 319) return GroupValueType::Binary;
    if (code >= 370 && code <= 389) return GroupValueType::Integer;
    if (code >= 400 && code <= 409) return GroupValueType::Integer;
    if (code >= 410 && code <= 419) return GroupValueType::Text;
    if (code >= 420 && code <= 429) return GroupValueType::Integer;
    if (code >= 430 && code <= 439) return GroupValueType::Text;
    if (code >= 440 && code <= 459) return GroupValueType::Integer;
    if (code >= 460 && code <= 469) return GroupValueType::Double;
    if (code >= 470 && code <= 479) return GroupValueType::Text;
    if (code >= 999 && code <= 1009) return GroupValueType::Text;
    if (code >= 1010 && code <= 1059) return GroupValueType::Double;
    if (code >= 1060 && code <= 1071) return GroupValueType::Integer;

    // Unknown codes are kept as text so nothing is lost.
    return GroupValueType::Text;
}

std::string DxfGroup::asString() const {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return std::to_string(*number);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*integer);
    }
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag ? "1" : "0";
    }
    return {};
}

double DxfGroup::asDouble() const noexcept {
    if (const auto* number = std::get_if<double>(&value)) {
        return *number;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*integer);
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        double parsed = 0.0;
        const auto [pointer, error] =
            std::from_chars(text->data(), text->data() + text->size(), parsed);
        return error == std::errc{} ? parsed : 0.0;
    }
    return 0.0;
}

std::int64_t DxfGroup::asInteger() const noexcept {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return *integer;
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(*number);
    }
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag ? 1 : 0;
    }
    return 0;
}

bool DxfGroup::asBool() const noexcept {
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag;
    }
    return asInteger() != 0;
}

DxfTokenizer::DxfTokenizer(std::string_view text) : text_(text) {
    // Strip a UTF-8 BOM, which otherwise makes the first group code
    // unparseable.
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
        static_cast<unsigned char>(text_[1]) == 0xBB &&
        static_cast<unsigned char>(text_[2]) == 0xBF) {
        text_.remove_prefix(3);
    }
}

std::optional<std::string_view> DxfTokenizer::readLine() {
    if (position_ >= text_.size()) {
        return std::nullopt;
    }

    const std::size_t start = position_;
    std::size_t end = start;
    while (end < text_.size() && text_[end] != '\n' && text_[end] != '\r') {
        ++end;
    }

    const std::string_view line = text_.substr(start, end - start);

    // Consume the terminator; CRLF counts as one.
    if (end < text_.size()) {
        if (text_[end] == '\r' && end + 1 < text_.size() && text_[end + 1] == '\n') {
            end += 2;
        } else {
            end += 1;
        }
    }
    position_ = end;
    ++lineNumber_;
    return line;
}

std::optional<GroupValue> DxfTokenizer::parseValue(int code, std::string_view raw) {
    switch (groupValueType(code)) {
        case GroupValueType::Text:
        case GroupValueType::Handle:
        case GroupValueType::Binary:
            // String values keep their internal whitespace: a text entity's
            // contents may legitimately begin or end with spaces.
            return GroupValue{std::string{raw}};

        case GroupValueType::Double: {
            const std::string_view trimmed = trim(raw);
            double parsed = 0.0;
            const auto [pointer, error] =
                std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
            if (error != std::errc{}) {
                diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                        "Group " + std::to_string(code) +
                                            ": expected a number, found '" +
                                            std::string{trimmed} + "'",
                                        lineNumber_ - 1});
                // Substituting 0 keeps the entity loadable; dropping the group
                // would shift a coordinate triple and misplace the geometry.
                return GroupValue{0.0};
            }
            return GroupValue{parsed};
        }

        case GroupValueType::Integer: {
            const std::string_view trimmed = trim(raw);
            std::int64_t parsed = 0;
            const auto [pointer, error] =
                std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
            if (error != std::errc{}) {
                diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                        "Group " + std::to_string(code) +
                                            ": expected an integer, found '" +
                                            std::string{trimmed} + "'",
                                        lineNumber_ - 1});
                return GroupValue{std::int64_t{0}};
            }
            return GroupValue{parsed};
        }

        case GroupValueType::Boolean:
            return GroupValue{trim(raw) != "0"};
    }
    return GroupValue{std::string{raw}};
}

std::optional<DxfGroup> DxfTokenizer::next() {
    for (;;) {
        const auto codeLine = readLine();
        if (!codeLine) {
            return std::nullopt;
        }

        const std::string_view trimmed = trim(*codeLine);
        // Blank lines between groups appear in files written by some tools.
        if (trimmed.empty()) {
            continue;
        }

        int code = 0;
        const auto [pointer, error] =
            std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), code);
        if (error != std::errc{} || pointer != trimmed.data() + trimmed.size()) {
            diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                    "Expected a group code, found '" + std::string{trimmed} +
                                        "'; skipping the pair",
                                    lineNumber_ - 1});
            // Drop the presumed value line, or the stream desynchronizes and
            // every subsequent group is misread.
            static_cast<void>(readLine());
            continue;
        }

        const auto valueLine = readLine();
        if (!valueLine) {
            diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                    "Group code " + std::to_string(code) +
                                        " at end of file has no value",
                                    lineNumber_});
            return std::nullopt;
        }

        auto parsed = parseValue(code, *valueLine);
        if (!parsed) {
            continue;
        }
        return DxfGroup{code, std::move(*parsed), lineNumber_ - 1};
    }
}

std::vector<DxfGroup> DxfTokenizer::readAll() {
    std::vector<DxfGroup> groups;
    while (auto group = next()) {
        groups.push_back(std::move(*group));
    }
    return groups;
}

std::optional<DxfGroup> GroupCursor::next() {
    if (pushedBack_) {
        auto group = std::move(*pushedBack_);
        pushedBack_.reset();
        return group;
    }
    return tokenizer_.next();
}

const DxfGroup* GroupCursor::peek() {
    if (!pushedBack_) {
        pushedBack_ = tokenizer_.next();
    }
    return pushedBack_ ? &*pushedBack_ : nullptr;
}

void GroupCursor::pushBack(DxfGroup group) {
    // Silently overwriting would drop a group and desynchronize the parse in a
    // way that surfaces far from the cause. One slot is all the grammar needs.
    if (pushedBack_) {
        throw std::logic_error("GroupCursor supports only a single group of pushback");
    }
    pushedBack_ = std::move(group);
}

std::string_view dxfVersionName(std::string_view code) noexcept {
    if (code == "AC1006") return "R10";
    if (code == "AC1009") return "R12";
    if (code == "AC1012") return "R13";
    if (code == "AC1014") return "R14";
    if (code == "AC1015") return "other CAD software";
    if (code == "AC1018") return "other CAD software";
    if (code == "AC1021") return "other CAD software";
    if (code == "AC1024") return "other CAD software";
    if (code == "AC1027") return "other CAD software";
    if (code == "AC1032") return "other CAD software";
    return code;
}

}  // namespace cad::io
