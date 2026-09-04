#include "cad/db/MTextFormat.hpp"

#include <charconv>
#include <cmath>
#include <vector>

#include "cad/db/TextEntities.hpp"
#include "cad/math/Angle.hpp"

namespace cad {
namespace {

/// The formatting in force at a point in the text.
struct State {
    double heightFactor = 1.0;
    double widthFactor = 1.0;
    double oblique = 0.0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool overline = false;
    bool strikethrough = false;
    std::optional<Color> color;
    std::string font;
    /// Codes seen but not modelled, verbatim, awaiting the next flush.
    std::string unmodelled;
};

[[nodiscard]] double toDouble(std::string_view text, double fallback) {
    double value = fallback;
    const auto [_, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} ? value : fallback;
}

/// A number without trailing zeros, because `\H1.500000x;` is noise in a file
/// a person may well open in a text editor.
[[nodiscard]] std::string trimmedNumber(double value) {
    std::string text = std::to_string(value);
    if (text.find('.') != std::string::npos) {
        while (text.size() > 1 && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text;
}

[[nodiscard]] long toInteger(std::string_view text, long fallback) {
    long value = fallback;
    const auto [_, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} ? value : fallback;
}

/// The `\f` argument: `Arial|b1|i0|c0|p34`.
void applyFont(State& state, std::string_view argument) {
    std::size_t start = 0;
    bool first = true;
    while (start <= argument.size()) {
        const std::size_t bar = argument.find('|', start);
        const std::string_view field =
            argument.substr(start, bar == std::string_view::npos ? std::string_view::npos
                                                                 : bar - start);
        if (first) {
            state.font = std::string{field};
            first = false;
        } else if (field.size() >= 2) {
            // Only the two that change how the glyphs are drawn. The codepage
            // and pitch fields describe the file's own font handling and mean
            // nothing to a renderer that asks the system for a family.
            if (field[0] == 'b') {
                state.bold = toInteger(field.substr(1), 0) != 0;
            } else if (field[0] == 'i') {
                state.italic = toInteger(field.substr(1), 0) != 0;
            }
        }
        if (bar == std::string_view::npos) {
            break;
        }
        start = bar + 1;
    }
}

}  // namespace

bool MTextRun::sameFormatting(const MTextRun& other) const noexcept {
    return heightFactor == other.heightFactor && widthFactor == other.widthFactor &&
           oblique == other.oblique && bold == other.bold && italic == other.italic &&
           underline == other.underline && overline == other.overline &&
           strikethrough == other.strikethrough && font == other.font &&
           color.has_value() == other.color.has_value() &&
           (!color.has_value() || color->equals(*other.color));
}

std::vector<MTextRun> parseMText(std::string_view contents) {
    std::vector<MTextRun> runs;
    std::vector<State> stack{State{}};
    std::string pending;

    const auto flush = [&](bool endsParagraph) {
        if (pending.empty() && !endsParagraph) {
            return;
        }
        const State& state = stack.back();
        MTextRun run;
        run.text = pending;
        run.heightFactor = state.heightFactor;
        run.widthFactor = state.widthFactor;
        run.oblique = state.oblique;
        run.bold = state.bold;
        run.italic = state.italic;
        run.underline = state.underline;
        run.overline = state.overline;
        run.strikethrough = state.strikethrough;
        run.color = state.color;
        run.font = state.font;
        run.unmodelledCodes = state.unmodelled;
        run.endsParagraph = endsParagraph;

        // Runs that read the same way are merged, so `{\C1;a}{\C1;b}` is one
        // run rather than two — the difference is invisible on screen and the
        // renderer would pay for it on every frame.
        if (!runs.empty() && !runs.back().endsParagraph && runs.back().sameFormatting(run) &&
            run.unmodelledCodes.empty()) {
            runs.back().text += run.text;
            runs.back().endsParagraph = endsParagraph;
        } else {
            runs.push_back(std::move(run));
        }
        // Claimed by the run just emitted; they must not be written twice.
        stack.back().unmodelled.clear();
        pending.clear();
    };

    for (std::size_t i = 0; i < contents.size(); ++i) {
        const char ch = contents[i];

        if (ch == '{') {
            flush(false);
            stack.push_back(stack.back());
            continue;
        }
        if (ch == '}') {
            flush(false);
            if (stack.size() > 1) {
                stack.pop_back();
            }
            continue;
        }
        if (ch != '\\' || i + 1 >= contents.size()) {
            pending.push_back(ch);
            continue;
        }

        const char next = contents[i + 1];

        // Escaped literals: the three characters that are otherwise markup.
        if (next == '\\' || next == '{' || next == '}') {
            pending.push_back(next);
            ++i;
            continue;
        }
        if (next == 'P') {
            flush(true);
            ++i;
            continue;
        }
        if (next == '~') {
            pending.push_back(' ');
            ++i;
            continue;
        }

        // Toggles, which carry no argument and no terminator.
        //
        // `\L` used to be treated as semicolon-terminated along with the codes
        // that are, so everything from an underline to the next semicolon —
        // usually the rest of the note — was swallowed. An underlined run
        // vanished entirely.
        const auto toggle = [&](bool& flag, bool on) {
            flush(false);
            flag = on;
            ++i;
        };
        State& state = stack.back();
        if (next == 'L') { toggle(state.underline, true); continue; }
        if (next == 'l') { toggle(state.underline, false); continue; }
        if (next == 'O') { toggle(state.overline, true); continue; }
        if (next == 'o') { toggle(state.overline, false); continue; }
        if (next == 'K') { toggle(state.strikethrough, true); continue; }
        if (next == 'k') { toggle(state.strikethrough, false); continue; }

        // Stacked text — a fraction or a tolerance. Rendered as `a/b`, which
        // is what it means; drawing it stacked needs a second baseline the
        // layout does not have yet.
        if (next == 'S') {
            const std::size_t end = contents.find(';', i + 2);
            std::string body{contents.substr(
                i + 2, end == std::string_view::npos ? std::string_view::npos : end - (i + 2))};
            if (const std::size_t separator = body.find_first_of("^#");
                separator != std::string::npos) {
                body[separator] = '/';
            }
            pending += body;
            i = end == std::string_view::npos ? contents.size() : end;
            continue;
        }

        // Everything else is `\X<argument>;`.
        const std::size_t end = contents.find(';', i + 2);
        const std::string_view argument = contents.substr(
            i + 2, end == std::string_view::npos ? std::string_view::npos : end - (i + 2));

        switch (next) {
            case 'H': {
                flush(false);
                // `\H2x;` is relative and `\H2;` is absolute — in the entity's
                // units, which a run cannot express, so it is taken as relative
                // to the entity's own height. Getting this backwards makes a
                // 2.5-unit note render at two and a half times its size.
                const bool relative = !argument.empty() && argument.back() == 'x';
                const std::string_view number =
                    relative ? argument.substr(0, argument.size() - 1) : argument;
                const double value = toDouble(number, 1.0);
                if (value > 0.0) {
                    stack.back().heightFactor = relative ? value : 1.0;
                }
                break;
            }
            case 'W': {
                flush(false);
                const double value = toDouble(argument, 1.0);
                if (value > 0.0) {
                    stack.back().widthFactor = value;
                }
                break;
            }
            case 'Q':
                flush(false);
                stack.back().oblique = degToRad(toDouble(argument, 0.0));
                break;
            case 'C': {
                flush(false);
                const long index = toInteger(argument, 256);
                // 256 is ByLayer and 0 is ByBlock; both mean "not this run's
                // business", which is exactly what an unset colour says.
                stack.back().color =
                    (index <= 0 || index >= 256)
                        ? std::optional<Color>{}
                        : std::optional<Color>{Color::fromIndex(static_cast<int>(index))};
                break;
            }
            case 'c': {
                flush(false);
                // A true colour, stored the way the format stores it: BGR.
                const auto packed = static_cast<std::uint32_t>(toInteger(argument, 0));
                const std::uint32_t rgb = ((packed & 0xFFu) << 16) | (packed & 0xFF00u) |
                                          ((packed >> 16) & 0xFFu);
                stack.back().color = Color::fromTrueColor(rgb);
                break;
            }
            case 'f':
            case 'F':
                flush(false);
                applyFont(stack.back(), argument);
                break;
            default:
                // A code this does not model. *Kept*, verbatim, so that a round
                // trip through an editor does not silently delete a paragraph
                // indent or a tracking value merely because nothing here can
                // show it.
                flush(false);
                stack.back().unmodelled.append(1, '\\');
                stack.back().unmodelled.append(1, next);
                stack.back().unmodelled.append(argument);
                stack.back().unmodelled.append(1, ';');
                break;
        }
        i = end == std::string_view::npos ? contents.size() : end;
    }

    flush(false);
    return runs;
}

std::string runsToPlainText(const std::vector<MTextRun>& runs) {
    std::string out;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        out += runs[i].text;
        if (runs[i].endsParagraph && i + 1 < runs.size()) {
            out.push_back('\n');
        }
    }
    return out;
}
std::string formatRuns(const std::vector<MTextRun>& runs, std::string_view defaultFont) {
    std::string out;

    for (std::size_t i = 0; i < runs.size(); ++i) {
        const MTextRun& run = runs[i];
        out += run.unmodelledCodes;

        std::string codes;
        if (run.heightFactor != 1.0) {
            codes += R"(\H)" + trimmedNumber(run.heightFactor) + "x;";
        }
        if (run.widthFactor != 1.0) {
            codes += R"(\W)" + trimmedNumber(run.widthFactor) + ";";
        }
        if (run.oblique != 0.0) {
            codes += R"(\Q)" + trimmedNumber(radToDeg(run.oblique)) + ";";
        }
        if (run.color) {
            // An indexed colour keeps its index, because that is what the rest
            // of the drawing means by a colour; anything else goes out as the
            // true colour the format stores, in its own BGR order.
            if (run.color->method() == ColorMethod::Index) {
                codes += R"(\C)" + std::to_string(run.color->index()) + ";";
            } else {
                const std::uint32_t rgb = run.color->toRgb();
                const std::uint32_t bgr =
                    ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu);
                codes += R"(\c)" + std::to_string(bgr) + ";";
            }
        }

        // Weight and slant are part of a font selection, not codes of their
        // own, so they need a family to be a weight *of*. With none available
        // they are dropped rather than written as something that will not
        // render — which is the same rule the parser follows in reverse.
        const std::string family = run.font.empty() ? std::string{defaultFont} : run.font;
        if (!family.empty() && (run.bold || run.italic || !run.font.empty())) {
            codes += R"(\f)" + family + "|b" + (run.bold ? "1" : "0") + "|i" +
                     (run.italic ? "1" : "0") + "|c0|p0;";
        }

        // The toggles wrap the text rather than preceding it, because that is
        // what a toggle means: it is turned back off at the end of the run.
        std::string body = MTextEntity::escapeForContents(run.text);
        if (run.underline) {
            body = R"(\L)" + body + R"(\l)";
        }
        if (run.overline) {
            body = R"(\O)" + body + R"(\o)";
        }
        if (run.strikethrough) {
            body = R"(\K)" + body + R"(\k)";
        }

        // Braced, so a run cannot leak its formatting into the next one —
        // which is the failure mode of emitting codes without scopes.
        out += codes.empty() ? body : "{" + codes + body + "}";
        if (run.endsParagraph) {
            out += R"(\P)";
        }
    }
    return out;
}

}  // namespace cad
