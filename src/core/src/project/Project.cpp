#include "cad/project/Project.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace cad {
namespace {

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

/// "a | b | c" into its parts, each trimmed. A field is never allowed to
/// hold the separator, so the split is exact.
[[nodiscard]] std::vector<std::string> fields(std::string_view line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t bar = line.find('|', start);
        out.push_back(trimmed(line.substr(start, bar == std::string_view::npos ? std::string_view::npos
                                                                               : bar - start)));
        if (bar == std::string_view::npos) {
            return out;
        }
        start = bar + 1;
    }
}

[[nodiscard]] std::string clean(std::string_view value) {
    // The separator and line breaks cannot be stored; a title with a bar in
    // it would come back as two fields.
    std::string out = trimmed(value);
    for (char& c : out) {
        if (c == '|' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return out;
}

[[nodiscard]] std::string field(const std::vector<std::string>& parts, std::size_t index) {
    return index < parts.size() ? parts[index] : std::string{};
}

}  // namespace

const ProjectSheet* Project::findSheet(std::string_view number) const {
    for (const ProjectSheet& sheet : sheets) {
        if (sheet.number == number) {
            return &sheet;
        }
    }
    return nullptr;
}

ProjectSheet* Project::findSheet(std::string_view number) {
    for (ProjectSheet& sheet : sheets) {
        if (sheet.number == number) {
            return &sheet;
        }
    }
    return nullptr;
}

const ProjectBackground* Project::findBackground(std::string_view wanted) const {
    for (const ProjectBackground& background : backgrounds) {
        if (background.name == wanted) {
            return &background;
        }
    }
    return nullptr;
}

void Project::sortSheets() {
    std::stable_sort(sheets.begin(), sheets.end(),
                     [](const ProjectSheet& a, const ProjectSheet& b) {
                         return sheetNumberLess(a.number, b.number);
                     });
}

bool sheetNumberLess(std::string_view a, std::string_view b) {
    // Walk both, comparing runs of digits as numbers and everything else
    // character by character without case.
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digitA = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool digitB = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (digitA && digitB) {
            std::size_t endA = i;
            while (endA < a.size() && std::isdigit(static_cast<unsigned char>(a[endA])) != 0) {
                ++endA;
            }
            std::size_t endB = j;
            while (endB < b.size() && std::isdigit(static_cast<unsigned char>(b[endB])) != 0) {
                ++endB;
            }
            const long long numberA = std::stoll(std::string(a.substr(i, endA - i)));
            const long long numberB = std::stoll(std::string(b.substr(j, endB - j)));
            if (numberA != numberB) {
                return numberA < numberB;
            }
            i = endA;
            j = endB;
            continue;
        }
        const char ca = static_cast<char>(std::toupper(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::toupper(static_cast<unsigned char>(b[j])));
        if (ca != cb) {
            return ca < cb;
        }
        ++i;
        ++j;
    }
    return (a.size() - i) < (b.size() - j);
}

Project parseProject(std::string_view text) {
    Project project;
    std::string body{text};
    std::istringstream stream{body};
    std::string raw;
    std::string notes;
    while (std::getline(stream, raw)) {
        const std::string line = trimmed(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = trimmed(line.substr(0, colon));
        const std::string value = trimmed(line.substr(colon + 1));
        if (key == "project") {
            project.name = value;
        } else if (key == "discipline") {
            project.discipline = value;
        } else if (key == "template") {
            project.sheetTemplate = value;
        } else if (key == "note") {
            notes += (notes.empty() ? "" : "\n") + value;
        } else if (key == "sheet") {
            const std::vector<std::string> parts = fields(value);
            ProjectSheet sheet;
            sheet.number = field(parts, 0);
            sheet.title = field(parts, 1);
            sheet.file = field(parts, 2);
            sheet.layout = field(parts, 3);
            sheet.background = field(parts, 4);
            sheet.status = field(parts, 5);
            if (!sheet.number.empty()) {
                project.sheets.push_back(std::move(sheet));
            }
        } else if (key == "background") {
            const std::vector<std::string> parts = fields(value);
            ProjectBackground background;
            background.name = field(parts, 0);
            background.file = field(parts, 1);
            if (!background.name.empty()) {
                project.backgrounds.push_back(std::move(background));
            }
        }
    }
    project.notes = notes;
    return project;
}

std::string formatProject(const Project& project) {
    std::ostringstream out;
    out << "# A Slate project: the sheets of a set and what stands behind them.\n"
           "# Made and kept by Slate; edit by hand if you like.\n";
    out << "project: " << clean(project.name) << "\n";
    if (!project.discipline.empty()) {
        out << "discipline: " << clean(project.discipline) << "\n";
    }
    if (!project.sheetTemplate.empty()) {
        out << "template: " << clean(project.sheetTemplate) << "\n";
    }
    {
        std::istringstream lines{project.notes};
        std::string line;
        while (std::getline(lines, line)) {
            if (!trimmed(line).empty()) {
                out << "note: " << clean(line) << "\n";
            }
        }
    }
    if (!project.backgrounds.empty()) {
        out << "\n# background: name | file\n";
        for (const ProjectBackground& background : project.backgrounds) {
            out << "background: " << clean(background.name) << " | " << clean(background.file)
                << "\n";
        }
    }
    if (!project.sheets.empty()) {
        out << "\n# sheet: number | title | file | layout | background | status\n";
        for (const ProjectSheet& sheet : project.sheets) {
            out << "sheet: " << clean(sheet.number) << " | " << clean(sheet.title) << " | "
                << clean(sheet.file) << " | " << clean(sheet.layout) << " | "
                << clean(sheet.background) << " | " << clean(sheet.status) << "\n";
        }
    }
    return out.str();
}

Project loadProject(const std::filesystem::path& path, std::string* error) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        if (error != nullptr) {
            *error = "Could not read " + path.string();
        }
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return parseProject(buffer.str());
}

bool saveProject(const Project& project, const std::filesystem::path& path, std::string* error) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        if (error != nullptr) {
            *error = "Could not write " + path.string();
        }
        return false;
    }
    out << formatProject(project);
    return static_cast<bool>(out);
}

}  // namespace cad
