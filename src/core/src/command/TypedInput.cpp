#include "cad/command/TypedInput.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <vector>

#include "cad/math/Angle.hpp"

namespace cad {
namespace {

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

[[nodiscard]] std::string upper(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

/// A number, or nothing. Whole-string: "12abc" is not a number, and accepting
/// it would turn a typo into a coordinate.
[[nodiscard]] std::optional<double> parseNumber(std::string_view text) {
    const std::string body = trimmed(text);
    if (body.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t used = 0;
        const double value = std::stod(body, &used);
        if (used != body.size()) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// `x,y` or `x,y,z`, optionally prefixed with `@` for "from the last point".
[[nodiscard]] std::optional<Vec3> parsePoint(std::string_view text, bool& relative) {
    std::string body = trimmed(text);
    relative = false;
    if (!body.empty() && body.front() == '@') {
        relative = true;
        body.erase(0, 1);
    }

    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t comma = body.find(',', start);
        const std::string piece =
            trimmed(body.substr(start, comma == std::string::npos ? std::string::npos
                                                                  : comma - start));
        if (!piece.empty()) {
            parts.push_back(piece);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    if (parts.size() < 2 || parts.size() > 3) {
        return std::nullopt;
    }
    const auto x = parseNumber(parts[0]);
    const auto y = parseNumber(parts[1]);
    if (!x || !y) {
        return std::nullopt;
    }
    double z = 0.0;
    if (parts.size() == 3) {
        const auto given = parseNumber(parts[2]);
        if (!given) {
            return std::nullopt;
        }
        z = *given;
    }
    return Vec3{*x, *y, z};
}

}  // namespace

const CommandKeyword* matchKeyword(const InputRequest& request, std::string_view text) {
    const std::string wanted = upper(trimmed(text));
    if (wanted.empty()) {
        return nullptr;
    }
    for (const CommandKeyword& keyword : request.keywords) {
        if (upper(keyword.shortcut) == wanted) {
            return &keyword;
        }
    }
    for (const CommandKeyword& keyword : request.keywords) {
        const std::string name = upper(keyword.name);
        if (name.rfind(wanted, 0) == 0) {
            return &keyword;
        }
    }
    return nullptr;
}

std::optional<InputValue> parseTypedInput(const InputRequest& request, std::string_view text,
                                          std::span<const Handle> selection) {
    if (trimmed(text).empty()) {
        // Enter. At a selection prompt it hands over what has been picked so
        // far; everywhere else it accepts the default or ends the command.
        if (request.kind == InputRequest::Kind::Selection) {
            return InputValue::makeSelection(
                std::vector<Handle>{selection.begin(), selection.end()});
        }
        return InputValue::makeEmpty();
    }

    if (const CommandKeyword* keyword = matchKeyword(request, text)) {
        return InputValue::makeKeyword(keyword->name);
    }

    switch (request.kind) {
        case InputRequest::Kind::Point: {
            bool relative = false;
            const auto point = parsePoint(text, relative);
            if (!point) {
                return std::nullopt;
            }
            const Vec3 absolute =
                relative && request.basePoint ? *request.basePoint + *point : *point;
            return InputValue::makePoint(absolute);
        }

        case InputRequest::Kind::Distance:
        case InputRequest::Kind::Number: {
            const auto value = parseNumber(text);
            return value ? std::optional{InputValue::makeNumber(*value)} : std::nullopt;
        }

        case InputRequest::Kind::Angle: {
            const auto degrees = parseNumber(text);
            if (!degrees) {
                return std::nullopt;
            }
            // Angles are typed in degrees and stored in radians; the boundary
            // conversion happens exactly here.
            return InputValue::makeNumber(degToRad(*degrees));
        }

        case InputRequest::Kind::Text:
        case InputRequest::Kind::Keyword:
            return InputValue::makeText(std::string{text});

        case InputRequest::Kind::Selection:
        case InputRequest::Kind::Entity:
            // Picked on the canvas, not typed.
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace cad
