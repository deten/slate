#include "cad/db/FieldEvaluator.hpp"

#include "cad/db/MTextFormat.hpp"
#include "cad/db/TableEntity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <unordered_map>

#include "cad/db/Database.hpp"
#include "cad/db/TextEntities.hpp"

namespace cad {
namespace {

/// Deepest field tree we will walk. A file naming a field from inside itself is
/// malformed, and the only wrong answer is to hang.
constexpr int kMaxFieldDepth = 16;

[[nodiscard]] std::string trim(std::string_view text) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto begin = std::find_if(text.begin(), text.end(), notSpace);
    const auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    return begin < end ? std::string{begin, end} : std::string{};
}

/// The integer following `marker` in `text`, e.g. the 3 in `%<\_FldIdx 3>%`.
[[nodiscard]] std::optional<std::size_t> indexAfter(std::string_view text, std::size_t from,
                                                    std::string_view marker) {
    const std::size_t at = text.find(marker, from);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t i = at + marker.size();
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
        ++i;
    }
    std::size_t value = 0;
    bool any = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
        value = value * 10 + static_cast<std::size_t>(text[i] - '0');
        ++i;
        any = true;
    }
    return any ? std::optional{value} : std::nullopt;
}

/// Strip the `\f "..."` format clause, leaving the expression.
[[nodiscard]] std::string withoutFormat(const std::string& fieldCode) {
    const std::size_t at = fieldCode.find("\\f ");
    return at == std::string::npos ? fieldCode : fieldCode.substr(0, at);
}

[[nodiscard]] std::string toUpper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

[[nodiscard]] std::string toLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

/// Insert thousands separators into the integer part of an already-formatted
/// number.
[[nodiscard]] std::string withSeparators(const std::string& number) {
    const std::size_t point = number.find('.');
    const std::size_t end = point == std::string::npos ? number.size() : point;
    std::size_t start = 0;
    if (!number.empty() && (number[0] == '-' || number[0] == '+')) {
        start = 1;
    }

    std::string out = number.substr(0, start);
    const std::size_t digits = end - start;
    for (std::size_t i = 0; i < digits; ++i) {
        if (i > 0 && (digits - i) % 3 == 0) {
            out += ',';
        }
        out += number[start + i];
    }
    out += number.substr(end);
    return out;
}

/// other CAD software property names that are not simply our display name.
///
/// Only the ones that genuinely differ. Everything else — Area, Length, Radius,
/// Diameter, Layer, Linetype — matches a registered property's display name,
/// including properties a plugin contributes, which is why the fallback below
/// matters more than this table.
[[nodiscard]] const std::unordered_map<std::string, std::string>& propertyAliases() {
    static const std::unordered_map<std::string, std::string> aliases{
        {"textstring", "text.contents"},
        {"contents", "text.contents"},
        {"textheight", "text.height"},
        {"height", "text.height"},
        {"rotation", "geometry.rotation"},
        {"position", "geometry.position"},
        {"startpoint", "geometry.start"},
        {"endpoint", "geometry.end"},
        {"center", "geometry.center"},
        {"centerpoint", "geometry.center"},
        {"name", "general.name"},
        {"layer", "general.layer"},
        {"linetype", "general.linetype"},
        {"color", "general.color"},
    };
    return aliases;
}

}  // namespace

// ---------------------------------------------------------------- formatting

FieldFormat FieldFormat::parse(const std::string& fieldCode) {
    const std::size_t clause = fieldCode.find("\\f ");
    if (clause == std::string::npos) {
        return {};
    }
    const std::size_t open = fieldCode.find('"', clause);
    if (open == std::string::npos) {
        return {};
    }
    const std::size_t close = fieldCode.find('"', open + 1);
    return parseSpec(fieldCode.substr(
        open + 1, close == std::string::npos ? std::string::npos : close - open - 1));
}

FieldFormat FieldFormat::parseSpec(const std::string& spec) {
    FieldFormat format;
    for (std::size_t i = 0; i < spec.size(); ++i) {
        if (spec[i] != '%' || i + 2 >= spec.size()) {
            continue;
        }
        const std::string key = spec.substr(i + 1, 2);
        std::size_t at = i + 3;

        const auto number = [&spec, &at]() {
            std::size_t start = at;
            while (at < spec.size() && std::isdigit(static_cast<unsigned char>(spec[at])) != 0) {
                ++at;
            }
            int value = 0;
            std::from_chars(spec.data() + start, spec.data() + at, value);
            return value;
        };
        // A bracketed argument, as in `%ct8[0.0069444444444444]`.
        const auto bracketed = [&spec, &at]() -> std::optional<std::string> {
            if (at >= spec.size() || spec[at] != '[') {
                return std::nullopt;
            }
            const std::size_t end = spec.find(']', at);
            if (end == std::string::npos) {
                return std::nullopt;
            }
            std::string inner = spec.substr(at + 1, end - at - 1);
            at = end + 1;
            return inner;
        };

        if (key == "pr") {
            format.precision = number();
        } else if (key == "lu") {
            // The unit *style*: decimal, engineering, architectural. Only the
            // separator flag is acted on here; a fractional-inch style is a
            // display convention this application does not yet offer, and
            // inventing one would misreport dimensions.
            const int style = number();
            format.separators = style == 3 || style == 5;
        } else if (key == "ct") {
            number();  // The conversion *kind*; the factor is what matters.
            if (const auto arg = bracketed()) {
                try {
                    format.conversion = std::stod(*arg);
                } catch (const std::exception&) {
                    format.conversion = 1.0;
                }
            }
        } else if (key == "tc") {
            format.textCase = number();
        } else if (key == "ps") {
            if (const auto prefix = bracketed()) {
                format.prefix = *prefix;
            }
            if (const auto suffix = bracketed()) {
                format.suffix = *suffix;
            }
        }
        i = at - 1;
    }
    return format;
}

std::string FieldFormat::applyToNumber(double value, int fallbackPrecision) const {
    const double converted = value * conversion;

    // A format that states no precision does not mean "as few digits as
    // possible" — it means the drawing's own, `LUPREC`. Guessing instead
    // produces `1912.96` where the drawing says `1912.958333`, which reads as a
    // rounding disagreement and is really a missing setting.
    const int places = std::clamp(precision.value_or(fallbackPrecision), 0, 20);
    std::string text;
    {
        std::array<char, 64> buffer{};
        const int written =
            std::snprintf(buffer.data(), buffer.size(), "%.*f", places, converted);
        text.assign(buffer.data(), static_cast<std::size_t>(std::max(0, written)));
    }

    if (separators) {
        text = withSeparators(text);
    }
    return applyToText(text);
}

std::string FieldFormat::applyToText(const std::string& value) const {
    std::string text = value;
    if (textCase == 1) {
        text = toUpper(std::move(text));
    } else if (textCase == 2) {
        text = toLower(std::move(text));
    }
    return prefix + text + suffix;
}

// ---------------------------------------------------------------- arithmetic

namespace {

/// A recursive-descent parser over `+ - * / ( )`.
///
/// Small enough to be obvious, which matters more than generality: this
/// evaluates numbers a drawing states, so a subtle precedence bug would put a
/// wrong dimension on a sheet.
class ArithmeticParser {
public:
    explicit ArithmeticParser(std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<double> parse() {
        const std::optional<double> value = expression();
        skipSpace();
        return at_ == text_.size() ? value : std::nullopt;
    }

private:
    void skipSpace() {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_])) != 0) {
            ++at_;
        }
    }

    [[nodiscard]] std::optional<double> expression() {
        std::optional<double> left = term();
        while (left) {
            skipSpace();
            if (at_ >= text_.size() || (text_[at_] != '+' && text_[at_] != '-')) {
                break;
            }
            const char op = text_[at_++];
            const std::optional<double> right = term();
            if (!right) {
                return std::nullopt;
            }
            left = op == '+' ? *left + *right : *left - *right;
        }
        return left;
    }

    [[nodiscard]] std::optional<double> term() {
        std::optional<double> left = factor();
        while (left) {
            skipSpace();
            if (at_ >= text_.size() || (text_[at_] != '*' && text_[at_] != '/')) {
                break;
            }
            const char op = text_[at_++];
            const std::optional<double> right = factor();
            if (!right) {
                return std::nullopt;
            }
            if (op == '/') {
                // Not an error to report and not a number to show. A field that
                // divides by zero has no value, so it keeps its cached one.
                if (*right == 0.0) {
                    return std::nullopt;
                }
                left = *left / *right;
            } else {
                left = *left * *right;
            }
        }
        return left;
    }

    [[nodiscard]] std::optional<double> factor() {
        skipSpace();
        if (at_ >= text_.size()) {
            return std::nullopt;
        }
        if (text_[at_] == '+' || text_[at_] == '-') {
            const char sign = text_[at_++];
            const std::optional<double> value = factor();
            if (!value) {
                return std::nullopt;
            }
            return sign == '-' ? -*value : *value;
        }
        if (text_[at_] == '(') {
            ++at_;
            const std::optional<double> value = expression();
            skipSpace();
            if (!value || at_ >= text_.size() || text_[at_] != ')') {
                return std::nullopt;
            }
            ++at_;
            return value;
        }

        const std::size_t start = at_;
        while (at_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[at_])) != 0 || text_[at_] == '.')) {
            ++at_;
        }
        if (at_ == start) {
            return std::nullopt;
        }
        try {
            return std::stod(std::string{text_.substr(start, at_ - start)});
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

}  // namespace

/// A number as an expression should see it: full precision, no formatting.
[[nodiscard]] std::string rawNumberText(double value) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
    return std::string{buffer.data(), static_cast<std::size_t>(std::max(0, written))};
}

std::optional<double> evaluateArithmetic(const std::string& expression) {
    return ArithmeticParser{expression}.parse();
}

// ----------------------------------------------------------------- evaluator

FieldEvaluator::FieldEvaluator(const Database& db, FieldContext context,
                               const PropertyRegistry& registry)
    : db_(&db), context_(std::move(context)), registry_(&registry) {}

const PropertyDefinition* FieldEvaluator::propertyFor(const DbObject& object,
                                                      const std::string& foreignName) const {
    const std::string key = toLower(foreignName);
    if (const auto alias = propertyAliases().find(key); alias != propertyAliases().end()) {
        if (const PropertyDefinition* definition = registry_->find(object, alias->second)) {
            return definition;
        }
    }
    for (const PropertyDefinition* definition : registry_->propertiesFor(object)) {
        if (toLower(definition->name) == key || toLower(definition->id) == key) {
            return definition;
        }
    }
    return nullptr;
}

std::optional<FieldResult> FieldEvaluator::evaluateFor(const DbObject& object) const {
    if (object.fieldHandle.isNull()) {
        return std::nullopt;
    }
    const auto* field = db_->get<FieldObject>(object.fieldHandle);
    if (field == nullptr) {
        return std::nullopt;
    }
    return evaluate(*field);
}

FieldResult FieldEvaluator::evaluate(const FieldObject& field) const {
    if (depth_ >= kMaxFieldDepth) {
        return {field.cachedValue, false, "field nests too deeply"};
    }
    ++depth_;
    const auto leave = [this] { --depth_; };

    const std::string evaluator = field.evaluatorName();
    FieldResult result;
    if (evaluator == "_text") {
        result = evaluateText(field);
    } else if (evaluator == "AcObjProp") {
        result = evaluateObjectProperty(field);
    } else if (evaluator == "AcExpr") {
        result = evaluateExpression(field);
    } else if (evaluator == "AcVar") {
        result = evaluateVariable(field);
    } else {
        result = {field.cachedValue, false, "unsupported field type " + field.evaluatorId};
    }
    leave();
    return result;
}

int FieldEvaluator::defaultPrecision() const {
    return db_->settings.linearPrecision;
}

std::string FieldEvaluator::expandChildren(const FieldObject& field, const std::string& text,
                                           bool raw, bool& ok, std::string& problem) const {
    // Rewritten left to right so a child's text cannot itself be rescanned for
    // holes — a value that happens to contain the marker is data, not code.
    static constexpr std::string_view kOpen = "%<";
    static constexpr std::string_view kClose = ">%";

    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t open = text.find(kOpen, at);
        if (open == std::string::npos) {
            out += text.substr(at);
            break;
        }
        const std::size_t close = text.find(kClose, open);
        if (close == std::string::npos) {
            out += text.substr(at);
            break;
        }
        out += text.substr(at, open - at);

        const std::string hole = text.substr(open, close + kClose.size() - open);
        const std::optional<std::size_t> index = indexAfter(hole, 0, "_FldIdx");
        if (index && *index < field.childFields.size()) {
            const auto* child = db_->get<FieldObject>(field.childFields[*index]);
            if (child != nullptr) {
                const FieldResult childResult = evaluate(*child);
                if (raw && childResult.number.has_value()) {
                    // Full precision, and none of the child's own formatting:
                    // an expression works on the value, not on what the value
                    // was going to look like.
                    std::array<char, 64> buffer{};
                    const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g",
                                                      *childResult.number);
                    out.append(buffer.data(), static_cast<std::size_t>(std::max(0, written)));
                } else if (raw || child->evaluatorName() == "_text") {
                    out += childResult.text;
                } else {
                    // The literal parts of a `_text` field are MText markup —
                    // that is how it carries its own line breaks — but a child's
                    // value is *data*. A room name that itself contains a `\P`
                    // has to arrive as those two characters and not as a break,
                    // or a label reading "MASTER\PBEDROOM" silently becomes two
                    // lines that were never in the drawing.
                    out += MTextEntity::escapeForContents(childResult.text);
                }
                ok = ok && childResult.evaluated;
                if (problem.empty() && !childResult.problem.empty()) {
                    problem = childResult.problem;
                }
            } else {
                ok = false;
                if (problem.empty()) {
                    problem = "field names a child that is not in the drawing";
                }
            }
        } else {
            // A hole we do not understand — an object index outside a property
            // reference, most likely. Leaving it in would print machine code on
            // the drawing, so the whole field falls back to its cache.
            ok = false;
            if (problem.empty()) {
                problem = "field code not understood: " + hole;
            }
        }
        at = close + kClose.size();
    }
    return out;
}

FieldResult FieldEvaluator::evaluateText(const FieldObject& field) const {
    bool ok = true;
    std::string problem;
    const std::string text =
        expandChildren(field, withoutFormat(field.fieldCode), /*raw=*/false, ok, problem);

    // A `_text` field is a template, and its own cache is usually empty — the
    // values live in the children. So a child this application cannot evaluate
    // does not blank the label: the child already substituted *its* cached
    // value, which is the last thing the drawing knew, and the template around
    // it is still correct. Only the flag changes.
    return {FieldFormat::parse(field.fieldCode).applyToText(text), ok, problem, std::nullopt};
}

FieldResult FieldEvaluator::evaluateObjectProperty(const FieldObject& field) const {
    const std::string code = withoutFormat(field.fieldCode);

    // `\AcObjProp Object(%<\_ObjIdx 0>%).Area`
    const std::optional<std::size_t> objectIndex = indexAfter(code, 0, "_ObjIdx");
    const std::size_t dot = code.rfind(").");
    if (!objectIndex || dot == std::string::npos ||
        *objectIndex >= field.referencedObjects.size()) {
        return {field.cachedValue, false, "field code not understood", std::nullopt};
    }
    const std::string propertyName = trim(std::string_view{code}.substr(dot + 2));

    const DbObject* target = db_->getObject(field.referencedObjects[*objectIndex]);
    if (target == nullptr || target->erased) {
        return {field.cachedValue, false, "field refers to an object that is gone",
                std::nullopt};
    }
    const PropertyDefinition* definition = propertyFor(*target, propertyName);
    if (definition == nullptr || !definition->get) {
        return {field.cachedValue, false,
                target->objectType() + " has no property " + propertyName, std::nullopt};
    }

    const FieldFormat format = FieldFormat::parse(field.fieldCode);
    const PropertyValue value = definition->get(*target);

    // The number carried out is the property's own, *before* the format's unit
    // conversion. An enclosing expression works in the drawing's units and does
    // its own converting, which is how a sum of square inches ends up stated in
    // square feet exactly once.
    if (const auto* number = std::get_if<double>(&value)) {
        return {format.applyToNumber(*number, defaultPrecision()), true, {}, *number};
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        const auto asDouble = static_cast<double>(*integer);
        return {format.applyToNumber(asDouble, defaultPrecision()), true, {}, asDouble};
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return {format.applyToText(*text), true, {}, std::nullopt};
    }
    if (const auto* flag = std::get_if<bool>(&value)) {
        return {format.applyToText(*flag ? "Yes" : "No"), true, {}, std::nullopt};
    }
    if (const auto* reference = std::get_if<Handle>(&value)) {
        // A reference property names a symbol table record, and its *name* is
        // what a drawing wants to say — "A-WALL", not a handle.
        if (const DbObject* record = db_->getObject(*reference)) {
            if (const PropertyDefinition* named = registry_->find(*record, "general.name");
                named != nullptr && named->get) {
                const PropertyValue recordName = named->get(*record);
                if (const auto* name = std::get_if<std::string>(&recordName)) {
                    return {format.applyToText(*name), true, {}, std::nullopt};
                }
            }
        }
        return {field.cachedValue, false, "field names a reference with no name", std::nullopt};
    }
    return {field.cachedValue, false, "field property has no text form", std::nullopt};
}

FieldResult FieldEvaluator::evaluate(const FieldObject& field, const TableEntity& table) const {
    const TableEntity* previous = table_;
    table_ = &table;
    FieldResult result = evaluate(field);
    table_ = previous;
    return result;
}

std::optional<double> FieldEvaluator::cellNumber(std::size_t row, std::size_t column) const {
    if (table_ == nullptr) {
        return std::nullopt;
    }
    const TableCell* cell = table_->cellAt(row, column);
    if (cell == nullptr) {
        return std::nullopt;
    }

    if (!cell->fieldHandle.isNull()) {
        if (const auto* field = db_->get<FieldObject>(cell->fieldHandle)) {
            const FieldResult result = evaluate(*field);
            if (result.number) {
                return result.number;
            }
            // A cell's field is usually a template holding one child -- the
            // colour code around the value is part of the cell, not of the
            // number -- so the value is one level down.
            if (field->childFields.size() == 1) {
                if (const auto* child = db_->get<FieldObject>(field->childFields.front())) {
                    const FieldResult inner = evaluate(*child);
                    if (inner.number) {
                        return inner.number;
                    }
                }
            }
        }
    }

    // A literal cell. Its codes are stripped first, because they contain
    // digits: a cell reading `{\C256;#####}` is an unresolved field wrapped in
    // a colour code, and scanning it for the first number finds the 256. Two
    // such cells in the sample drawing's new-area schedule put its total out by
    // exactly 512 square inches, which is a plausible-looking 3.56 square feet.
    const std::string text = runsToPlainText(parseMText(cell->displayText()));
    std::size_t start = text.find_first_of("-.0123456789");
    while (start != std::string::npos) {
        const char* begin = text.data() + start;
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end != begin) {
            return value;
        }
        start = text.find_first_of("-.0123456789", start + 1);
    }
    return std::nullopt;
}

std::string FieldEvaluator::substituteCellFunctions(const std::string& text, bool& ok,
                                                    std::string& problem) const {
    // A1-style reference: one or more letters, then a row number counted from
    // one over the whole grid -- the title row included, which is what the
    // sample drawing's own totals confirm.
    const auto readReference = [&text](std::size_t& at, std::size_t& row,
                                       std::size_t& column) -> bool {
        std::size_t i = at;
        std::size_t letters = 0;
        std::size_t columnIndex = 0;
        while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i])) != 0) {
            const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
            columnIndex = columnIndex * 26 + static_cast<std::size_t>(upper - 'A' + 1);
            ++letters;
            ++i;
        }
        if (letters == 0 || i >= text.size() ||
            std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
        std::size_t rowNumber = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            rowNumber = rowNumber * 10 + static_cast<std::size_t>(text[i] - '0');
            ++i;
        }
        if (rowNumber == 0) {
            return false;
        }
        row = rowNumber - 1;
        column = columnIndex - 1;
        at = i;
        return true;
    };

    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (std::isalpha(static_cast<unsigned char>(text[i])) == 0) {
            out += text[i++];
            continue;
        }

        // A name followed by `(` is a function over a range; a bare reference
        // is a single cell.
        std::size_t nameEnd = i;
        while (nameEnd < text.size() &&
               std::isalpha(static_cast<unsigned char>(text[nameEnd])) != 0) {
            ++nameEnd;
        }
        std::string name = text.substr(i, nameEnd - i);
        for (char& c : name) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        const bool isFunction = nameEnd < text.size() && text[nameEnd] == '(' &&
                                (name == "SUM" || name == "AVERAGE" || name == "AVG" ||
                                 name == "COUNT" || name == "MIN" || name == "MAX");
        if (!isFunction) {
            std::size_t at = i;
            std::size_t row = 0;
            std::size_t column = 0;
            if (readReference(at, row, column)) {
                if (table_ == nullptr) {
                    ok = false;
                    problem = "a cell reference outside a table";
                    return text;
                }
                const std::optional<double> value = cellNumber(row, column);
                out += rawNumberText(value.value_or(0.0));
                i = at;
                continue;
            }
            out += text.substr(i, nameEnd - i);
            i = nameEnd;
            continue;
        }

        if (table_ == nullptr) {
            ok = false;
            problem = "a cell formula outside a table";
            return text;
        }

        std::size_t at = nameEnd + 1;
        std::size_t firstRow = 0;
        std::size_t firstColumn = 0;
        if (!readReference(at, firstRow, firstColumn)) {
            ok = false;
            problem = "unreadable cell reference in " + name;
            return text;
        }
        std::size_t lastRow = firstRow;
        std::size_t lastColumn = firstColumn;
        if (at < text.size() && text[at] == ':') {
            ++at;
            if (!readReference(at, lastRow, lastColumn)) {
                ok = false;
                problem = "unreadable cell range in " + name;
                return text;
            }
        }
        if (at >= text.size() || text[at] != ')') {
            ok = false;
            problem = name + " is missing its closing bracket";
            return text;
        }
        ++at;

        std::vector<double> values;
        for (std::size_t row = std::min(firstRow, lastRow); row <= std::max(firstRow, lastRow);
             ++row) {
            for (std::size_t column = std::min(firstColumn, lastColumn);
                 column <= std::max(firstColumn, lastColumn); ++column) {
                // A blank or non-numeric cell contributes nothing rather than
                // failing the sum: a schedule with a spacer row in the middle
                // of its range is normal, and the drawing's own total counts it
                // as zero.
                if (const std::optional<double> value = cellNumber(row, column)) {
                    values.push_back(*value);
                }
            }
        }

        double result = 0.0;
        if (name == "SUM") {
            for (const double value : values) {
                result += value;
            }
        } else if (name == "COUNT") {
            result = static_cast<double>(values.size());
        } else if (name == "AVERAGE" || name == "AVG") {
            for (const double value : values) {
                result += value;
            }
            result = values.empty() ? 0.0 : result / static_cast<double>(values.size());
        } else if (name == "MIN") {
            result = values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
        } else if (name == "MAX") {
            result = values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
        }

        out += rawNumberText(result);
        i = at;
    }
    return out;
}

FieldResult FieldEvaluator::evaluateExpression(const FieldObject& field) const {
    bool ok = true;
    std::string problem;
    const std::string expanded =
        expandChildren(field, withoutFormat(field.fieldCode), /*raw=*/true, ok, problem);
    if (!ok) {
        return {field.cachedValue, false, problem, std::nullopt};
    }

    // Drop the leading `\AcExpr`, leaving the parenthesised expression.
    std::string text = expanded;
    if (const std::size_t space = text.find(' '); text.starts_with("\\Ac") &&
                                                  space != std::string::npos) {
        text = text.substr(space + 1);
    }

    // Cell formulas first: a schedule's total is arithmetic over its grid, and
    // the arithmetic cannot begin until the grid has been read.
    bool cellsOk = true;
    std::string cellProblem;
    text = substituteCellFunctions(text, cellsOk, cellProblem);
    if (!cellsOk) {
        return {field.cachedValue, false, cellProblem, std::nullopt};
    }

    const std::optional<double> value = evaluateArithmetic(text);
    if (!value) {
        return {field.cachedValue, false, "expression not understood: " + trim(text),
                std::nullopt};
    }

    // An expression over a *single* value states no new quantity — it rescales
    // one — so when it gives no precision of its own it keeps that value's.
    // `(area/144) \f "%lu2"` over a child asking for two decimals shows two,
    // not the drawing's six, which would print a square-foot area to a
    // millionth of a foot. Combine two values and the result is something new,
    // and the drawing's own precision applies.
    //
    // Inferred from the sample drawing rather than from a specification: it is
    // the rule that makes all eighty-seven of its fields agree with what
    // other CAD software last wrote, and no other reading of the codes does.
    int precision = defaultPrecision();
    // The format the authoring application applied, when it recorded one. A
    // field code stating a unit style and no precision is not saying "use the
    // drawing's" -- the precision it used is in the value block beside the
    // number, and for a schedule total it is the difference between 1142.30 and
    // 1142.297016.
    if (const auto applied = FieldFormat::parseSpec(field.appliedFormat).precision) {
        precision = *applied;
    } else if (field.childFields.size() == 1) {
        if (const auto* child = db_->get<FieldObject>(field.childFields.front())) {
            if (const auto stated = FieldFormat::parse(child->fieldCode).precision) {
                precision = *stated;
            }
        }
    }

    return {FieldFormat::parse(field.fieldCode).applyToNumber(*value, precision), true, {},
            *value};
}

FieldResult FieldEvaluator::evaluateVariable(const FieldObject& field) const {
    std::string text = withoutFormat(field.fieldCode);
    if (const std::size_t space = text.find(' '); space != std::string::npos) {
        text = trim(std::string_view{text}.substr(space + 1));
    }
    const std::string name = toLower(text);
    const FieldFormat format = FieldFormat::parse(field.fieldCode);

    if (name == "filename") {
        return {format.applyToText(context_.fileName), true, {}};
    }
    if (name == "filepath" || name == "fullfilename") {
        return {format.applyToText(context_.filePath), true, {}};
    }
    if (name == "pagesetupname" || name == "layoutname" || name == "sheetname") {
        return {format.applyToText(context_.layoutName), true, {}};
    }
    if (name == "date" || name == "createdate" || name == "savedate") {
        if (context_.currentTime == 0) {
            return {field.cachedValue, false, "no clock supplied for a date field"};
        }
        const auto seconds = static_cast<std::time_t>(context_.currentTime);
        std::tm parts{};
#ifdef _WIN32
        gmtime_s(&parts, &seconds);
#else
        gmtime_r(&seconds, &parts);
#endif
        std::array<char, 64> buffer{};
        const std::size_t written =
            std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d", &parts);
        return {format.applyToText(std::string{buffer.data(), written}), true, {}};
    }
    return {field.cachedValue, false, "unsupported field variable " + text};
}

std::size_t refreshTableCells(Database& db, const FieldContext& context) {
    const FieldEvaluator evaluator{db, context};
    std::size_t resolved = 0;

    for (const Handle blockHandle : db.blocks().handles()) {
        auto* block = db.get<BlockTableRecord>(blockHandle);
        if (block == nullptr) {
            continue;
        }
        for (Entity* entity : db.entitiesIn(*block)) {
            auto* table = dynamic_cast<TableEntity*>(entity);
            if (table == nullptr || table->erased) {
                continue;
            }
            for (TableCell& cell : table->cells) {
                if (cell.fieldHandle.isNull()) {
                    continue;
                }
                const auto* field = db.get<FieldObject>(cell.fieldHandle);
                if (field == nullptr) {
                    continue;
                }
                // The evaluated value when it evaluates, and the cached one
                // when it does not — the same rule the rest of the field code
                // follows, because a stale number on a schedule is worth far
                // more than a blank one.
                const FieldResult result = evaluator.evaluate(*field, *table);
                cell.resolvedText = result.text;
                if (!cell.resolvedText.empty()) {
                    ++resolved;
                }
            }
        }
    }
    return resolved;
}

}  // namespace cad
