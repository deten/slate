#include "cad/config/ConfigFile.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

namespace cad {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) {
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

[[nodiscard]] std::string leadingWhitespace(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
        ++i;
    }
    return std::string{text.substr(0, i)};
}

[[nodiscard]] bool isCommentOrBlank(std::string_view trimmed) {
    return trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';';
}

}  // namespace

std::string normalizeConfigName(std::string_view name) {
    std::string out{trim(name)};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

ConfigFile ConfigFile::parse(std::string_view text) {
    ConfigFile config;
    std::string currentSection;

    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        const bool last = end == std::string_view::npos;
        if (last) {
            end = text.size();
        }
        std::string_view raw = text.substr(start, end - start);
        // Tolerate CRLF without carrying the carriage return into values.
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }
        start = end + 1;

        // A trailing newline ends the last line rather than starting an empty
        // one, which would grow the file by a blank line on every save.
        if (last && raw.empty() && !config.lines_.empty()) {
            break;
        }

        Line line;
        line.raw = std::string{raw};
        line.section = currentSection;

        const std::string_view trimmed = trim(raw);
        if (isCommentOrBlank(trimmed)) {
            config.lines_.push_back(std::move(line));
            if (last) {
                break;
            }
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            currentSection = normalizeConfigName(trimmed.substr(1, trimmed.size() - 2));
            line.kind = Line::Kind::Section;
            line.section = currentSection;
            config.lines_.push_back(std::move(line));
            if (last) {
                break;
            }
            continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string_view::npos) {
            // Not a key/value pair and not a comment. Kept verbatim rather than
            // dropped, so a typo costs the user nothing but the setting.
            config.lines_.push_back(std::move(line));
            if (last) {
                break;
            }
            continue;
        }

        const std::string_view keyText = trim(trimmed.substr(0, equals));
        if (keyText.empty()) {
            config.lines_.push_back(std::move(line));
            if (last) {
                break;
            }
            continue;
        }

        line.kind = Line::Kind::Entry;
        line.indent = leadingWhitespace(raw);
        line.keyAsWritten = std::string{keyText};
        line.key = normalizeConfigName(keyText);
        line.value = std::string{trim(trimmed.substr(equals + 1))};
        config.lines_.push_back(std::move(line));

        if (last) {
            break;
        }
    }
    return config;
}

ConfigFile ConfigFile::load(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return ConfigFile{};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str());
}

std::string ConfigFile::toText() const {
    std::string out;
    for (const Line& line : lines_) {
        if (line.kind == Line::Kind::Entry && line.edited) {
            out += line.indent;
            out += line.keyAsWritten;
            out += " = ";
            out += line.value;
        } else {
            out += line.raw;
        }
        out += '\n';
    }
    return out;
}

bool ConfigFile::save(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return false;
    }
    const std::string text = toText();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

const ConfigFile::Line* ConfigFile::find(std::string_view section, std::string_view key) const {
    const std::string wantedSection = normalizeConfigName(section);
    const std::string wantedKey = normalizeConfigName(key);
    for (const Line& line : lines_) {
        if (line.kind == Line::Kind::Entry && line.section == wantedSection &&
            line.key == wantedKey) {
            return &line;
        }
    }
    return nullptr;
}

ConfigFile::Line* ConfigFile::find(std::string_view section, std::string_view key) {
    return const_cast<Line*>(std::as_const(*this).find(section, key));
}

bool ConfigFile::has(std::string_view section, std::string_view key) const {
    return find(section, key) != nullptr;
}

std::optional<std::string> ConfigFile::get(std::string_view section,
                                           std::string_view key) const {
    const Line* line = find(section, key);
    return line == nullptr ? std::nullopt : std::optional{line->value};
}

std::string ConfigFile::text(std::string_view section, std::string_view key,
                             std::string fallback) const {
    const Line* line = find(section, key);
    return line == nullptr ? std::move(fallback) : line->value;
}

double ConfigFile::number(std::string_view section, std::string_view key,
                          double fallback) const {
    const Line* line = find(section, key);
    if (line == nullptr) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(line->value, &consumed);
        // Trailing rubbish means the user meant something else; the fallback is
        // safer than half a number.
        return consumed == line->value.size() ? parsed : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

long long ConfigFile::integer(std::string_view section, std::string_view key,
                              long long fallback) const {
    const Line* line = find(section, key);
    if (line == nullptr) {
        return fallback;
    }
    long long parsed = 0;
    const char* begin = line->value.data();
    const char* end = begin + line->value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fallback;
    }
    return parsed;
}

bool ConfigFile::boolean(std::string_view section, std::string_view key, bool fallback) const {
    const Line* line = find(section, key);
    if (line == nullptr) {
        return fallback;
    }
    const std::string value = normalizeConfigName(line->value);
    if (value == "true" || value == "yes" || value == "on" || value == "1") {
        return true;
    }
    if (value == "false" || value == "no" || value == "off" || value == "0") {
        return false;
    }
    return fallback;
}

std::size_t ConfigFile::appendPositionFor(std::string_view section) const {
    const std::string wanted = normalizeConfigName(section);
    std::size_t position = 0;
    bool found = false;

    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].section == wanted) {
            found = true;
            position = i + 1;
        } else if (found) {
            break;
        }
    }
    if (!found) {
        return lines_.size();
    }

    // Back up over trailing blank lines so the new key joins the section's
    // content rather than landing after the gap before the next heading.
    while (position > 0 && lines_[position - 1].kind == ConfigFile::Line::Kind::Other &&
           trim(lines_[position - 1].raw).empty()) {
        --position;
    }
    return position;
}

void ConfigFile::set(std::string_view section, std::string_view key, std::string_view value) {
    if (Line* existing = find(section, key)) {
        existing->value = std::string{value};
        existing->edited = true;
        return;
    }

    const std::string normalizedSection = normalizeConfigName(section);
    const bool sectionExists =
        std::any_of(lines_.begin(), lines_.end(), [&](const Line& line) {
            return line.kind == Line::Kind::Section && line.section == normalizedSection;
        });

    Line entry;
    entry.kind = Line::Kind::Entry;
    entry.section = normalizedSection;
    entry.key = normalizeConfigName(key);
    entry.keyAsWritten = std::string{key};
    entry.value = std::string{value};
    // New lines have no original text to preserve, so they are always written
    // from their fields.
    entry.edited = true;

    if (!sectionExists) {
        // A blank line before a new heading, unless the file is empty, so an
        // appended section does not run into what came before it.
        if (!lines_.empty()) {
            Line spacer;
            spacer.section = normalizedSection;
            lines_.push_back(std::move(spacer));
        }
        Line header;
        header.kind = Line::Kind::Section;
        header.section = normalizedSection;
        header.raw = "[" + std::string{section} + "]";
        lines_.push_back(std::move(header));
        lines_.push_back(std::move(entry));
        return;
    }

    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(appendPositionFor(section)),
                  std::move(entry));
}

void ConfigFile::setNumber(std::string_view section, std::string_view key, double value) {
    std::ostringstream out;
    out << value;
    set(section, key, out.str());
}

void ConfigFile::setBoolean(std::string_view section, std::string_view key, bool value) {
    set(section, key, value ? "true" : "false");
}

bool ConfigFile::remove(std::string_view section, std::string_view key) {
    const std::string wantedSection = normalizeConfigName(section);
    const std::string wantedKey = normalizeConfigName(key);
    const auto it = std::find_if(lines_.begin(), lines_.end(), [&](const Line& line) {
        return line.kind == Line::Kind::Entry && line.section == wantedSection &&
               line.key == wantedKey;
    });
    if (it == lines_.end()) {
        return false;
    }
    lines_.erase(it);
    return true;
}

std::vector<std::pair<std::string, std::string>> ConfigFile::entries(
    std::string_view section) const {
    const std::string wanted = normalizeConfigName(section);
    std::vector<std::pair<std::string, std::string>> out;
    for (const Line& line : lines_) {
        if (line.kind == Line::Kind::Entry && line.section == wanted) {
            out.emplace_back(line.keyAsWritten, line.value);
        }
    }
    return out;
}

std::vector<std::string> ConfigFile::sections() const {
    std::vector<std::string> out;
    for (const Line& line : lines_) {
        if (line.kind == Line::Kind::Section &&
            std::find(out.begin(), out.end(), line.section) == out.end()) {
            out.push_back(line.section);
        }
    }
    return out;
}

}  // namespace cad
