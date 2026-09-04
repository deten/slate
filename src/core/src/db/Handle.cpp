#include "cad/db/Handle.hpp"

#include <charconv>
#include <cctype>

namespace cad {
namespace {

/// Trim leading and trailing ASCII whitespace.
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

std::string Handle::toString() const {
    // Uppercase hex with no leading zeros, matching what other CAD software writes.
    static constexpr char kDigits[] = "0123456789ABCDEF";
    if (value_ == 0) {
        return "0";
    }

    char buffer[17];
    int index = 16;
    std::uint64_t remaining = value_;
    while (remaining != 0) {
        buffer[--index] = kDigits[remaining & 0xF];
        remaining >>= 4;
    }
    return std::string{buffer + index, static_cast<std::size_t>(16 - index)};
}

std::optional<Handle> Handle::tryParse(std::string_view text) noexcept {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::uint64_t parsed = 0;
    const char* const begin = trimmed.data();
    const char* const end = begin + trimmed.size();
    const auto [pointer, error] = std::from_chars(begin, end, parsed, 16);

    // The whole string must be consumed: "1EZ" is malformed, not 0x1E.
    if (error != std::errc{} || pointer != end) {
        return std::nullopt;
    }
    return Handle{parsed};
}

Handle Handle::parse(std::string_view text) {
    const auto result = tryParse(text);
    if (!result) {
        throw std::invalid_argument("Invalid handle: " + std::string{text});
    }
    return *result;
}

}  // namespace cad
