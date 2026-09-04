#include "AbiInternal.hpp"

#include <cstring>

namespace cad::abi::detail {

// ------------------------------------------------------------------- errors

std::string& errorSlot() noexcept {
    thread_local std::string message;
    return message;
}

CadStatus fail(CadStatus status, std::string message) {
    errorSlot() = std::move(message);
    return status;
}

// -------------------------------------------------------------- conversions

CadBox toCadBox(const Box3& box) noexcept {
    return CadBox{toCadPoint(box.min), toCadPoint(box.max)};
}

Color toColor(const CadColor& color) noexcept {
    switch (color.method) {
        case CAD_COLOR_BY_BLOCK:
            return Color::byBlock();
        case CAD_COLOR_INDEX:
            return Color::fromIndex(color.index);
        case CAD_COLOR_TRUE:
            return Color::fromTrueColor(color.rgb);
        case CAD_COLOR_BY_LAYER:
        default:
            return Color::byLayer();
    }
}

CadColor toCadColor(const Color& color) noexcept {
    CadColor out{};
    switch (color.method()) {
        case ColorMethod::ByBlock:
            out.method = CAD_COLOR_BY_BLOCK;
            break;
        case ColorMethod::Index:
            out.method = CAD_COLOR_INDEX;
            out.index = color.index();
            // The RGB is filled in as well, even though the method says index.
            // A plugin that only wants to draw something has the answer without
            // needing to carry a copy of the ACI palette.
            out.rgb = color.toRgb();
            break;
        case ColorMethod::TrueColor:
            out.method = CAD_COLOR_TRUE;
            out.rgb = color.rgb();
            break;
        case ColorMethod::ByLayer:
        default:
            out.method = CAD_COLOR_BY_LAYER;
            break;
    }
    return out;
}

CadStatus copyString(std::string_view text, char* buffer, std::size_t capacity,
                     std::size_t* outRequired) {
    const std::size_t required = text.size() + 1;
    if (outRequired != nullptr) { *outRequired = required; }
    if (buffer == nullptr) {
        // Sizing call. Not an error: it is half of the documented two-call
        // pattern, and reporting failure here would make every correct caller
        // look like it had failed once.
        return capacity == 0 ? CAD_OK : CAD_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < required) {
        return fail(CAD_ERROR_BUFFER_TOO_SMALL, "buffer of " + std::to_string(capacity) +
                                                    " bytes needs " + std::to_string(required));
    }
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    return CAD_OK;
}

CadStatus copyHandles(const std::vector<Handle>& handles, CadHandle* buffer,
                      std::uint32_t capacity, std::uint32_t* outCount) {
    const auto required = static_cast<std::uint32_t>(handles.size());
    if (outCount != nullptr) { *outCount = required; }
    if (buffer == nullptr) { return CAD_OK; }
    if (capacity < required) {
        return fail(CAD_ERROR_BUFFER_TOO_SMALL, "array of " + std::to_string(capacity) +
                                                    " needs " + std::to_string(required));
    }
    for (std::uint32_t i = 0; i < required; ++i) { buffer[i] = handles[i].value(); }
    return CAD_OK;
}

// ------------------------------------------------------------ step results

namespace {

/// Copy a possibly-null C string. Null is an absent string, not an error: most
/// of the optional fields in the ABI are expressed by leaving a pointer null.
[[nodiscard]] std::string copyOptional(const char* text) {
    return text != nullptr ? std::string{text} : std::string{};
}

[[nodiscard]] InputRequest::Kind toRequestKind(std::int32_t kind) noexcept {
    switch (kind) {
        case CAD_INPUT_DISTANCE: return InputRequest::Kind::Distance;
        case CAD_INPUT_ANGLE: return InputRequest::Kind::Angle;
        case CAD_INPUT_NUMBER: return InputRequest::Kind::Number;
        case CAD_INPUT_TEXT: return InputRequest::Kind::Text;
        case CAD_INPUT_SELECTION: return InputRequest::Kind::Selection;
        case CAD_INPUT_ENTITY: return InputRequest::Kind::Entity;
        case CAD_INPUT_KEYWORD: return InputRequest::Kind::Keyword;
        case CAD_INPUT_POINT:
        default: return InputRequest::Kind::Point;
    }
}

[[nodiscard]] InputRequest toInputRequest(const CadInputRequest& source) {
    InputRequest request;
    if (!includes(source.struct_size, offsetof(CadInputRequest, kind) + sizeof(std::int32_t))) {
        // Too small to carry even a kind. Returning the default point prompt is
        // the only non-crashing option, and the caller's step conversion has
        // already reported the malformed size.
        return request;
    }
    request.kind = toRequestKind(source.kind);

    if (CAD_ABI_HAS(source.struct_size, CadInputRequest, prompt)) {
        request.prompt = copyOptional(source.prompt);
    }
    if (CAD_ABI_HAS(source.struct_size, CadInputRequest, base_point) &&
        source.has_base_point != 0) {
        request.basePoint = toVec3(source.base_point);
    }
    if (CAD_ABI_HAS(source.struct_size, CadInputRequest, keyword_count) &&
        source.keywords != nullptr) {
        request.keywords.reserve(source.keyword_count);
        for (std::uint32_t i = 0; i < source.keyword_count; ++i) {
            const CadKeyword& keyword = source.keywords[i];
            request.keywords.push_back(CommandKeyword{copyOptional(keyword.name),
                                                      copyOptional(keyword.shortcut),
                                                      copyOptional(keyword.description)});
        }
    }
    if (CAD_ABI_HAS(source.struct_size, CadInputRequest, default_number) &&
        source.has_default_number != 0) {
        request.defaultNumber = source.default_number;
    }
    if (CAD_ABI_HAS(source.struct_size, CadInputRequest, allow_empty)) {
        request.allowEmpty = source.allow_empty != 0;
    }
    return request;
}

}  // namespace

StepResult toStepResult(const CadStepResult& result) {
    if (!includes(result.struct_size, offsetof(CadStepResult, kind) + sizeof(std::int32_t))) {
        return StepResult::failed(
            "plugin returned a step result with an invalid struct_size; the field is mandatory");
    }

    std::string message;
    if (CAD_ABI_HAS(result.struct_size, CadStepResult, message)) {
        message = copyOptional(result.message);
    }

    switch (result.kind) {
        case CAD_STEP_REQUEST: {
            if (!CAD_ABI_HAS(result.struct_size, CadStepResult, request)) {
                return StepResult::failed("plugin asked for input but omitted the request");
            }
            return StepResult::ask(toInputRequest(result.request));
        }
        case CAD_STEP_CANCELLED:
            return StepResult::cancelled(std::move(message));
        case CAD_STEP_FAILED:
            // A failure with no explanation is worse than useless in a command
            // line, so give it one rather than printing an empty line.
            return StepResult::failed(message.empty() ? "the command failed" : std::move(message));
        case CAD_STEP_DONE:
        default:
            return StepResult::done(std::move(message));
    }
}

CadInputValue toCadInputValue(const InputValue& input, std::vector<CadHandle>& scratch) {
    CadInputValue value{};
    value.struct_size = static_cast<std::uint32_t>(sizeof(CadInputValue));

    switch (input.kind) {
        case InputValue::Kind::Point: value.kind = CAD_VALUE_POINT; break;
        case InputValue::Kind::Number: value.kind = CAD_VALUE_NUMBER; break;
        case InputValue::Kind::Text: value.kind = CAD_VALUE_TEXT; break;
        case InputValue::Kind::Selection: value.kind = CAD_VALUE_SELECTION; break;
        case InputValue::Kind::Entity: value.kind = CAD_VALUE_ENTITY; break;
        case InputValue::Kind::Keyword: value.kind = CAD_VALUE_KEYWORD; break;
        case InputValue::Kind::Cancel: value.kind = CAD_VALUE_CANCEL; break;
        case InputValue::Kind::Empty:
        default: value.kind = CAD_VALUE_EMPTY; break;
    }

    // Every field is populated regardless of kind. The values are already
    // there, and a plugin that reads `point` after a keyword gets the origin
    // rather than uninitialised memory.
    value.point = toCadPoint(input.point);
    value.number = input.number;
    value.text = input.text.c_str();
    value.entity = input.entity.value();

    scratch.clear();
    scratch.reserve(input.selection.size());
    for (const Handle handle : input.selection) { scratch.push_back(handle.value()); }
    value.selection_count = static_cast<std::uint32_t>(scratch.size());
    value.selection = scratch.empty() ? nullptr : scratch.data();
    return value;
}

// ------------------------------------------------------------ command bridge

AbiCommandInstance::AbiCommandInstance(const OwnedCommandDesc& desc) : desc_(desc) {
    if (desc_.onCreate != nullptr) {
        instance_ = desc_.onCreate(desc_.userData);
        ownsInstance_ = true;
    } else {
        instance_ = desc_.userData;
    }
}

AbiCommandInstance::~AbiCommandInstance() {
    if (ownsInstance_ && desc_.onDestroy != nullptr) {
        // A destructor must not let an exception escape, and this one calls
        // into foreign code. If a plugin throws through a C callback there is
        // nothing useful left to do, but terminating the application over it
        // would be the wrong trade.
        try {
            desc_.onDestroy(instance_);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }
}

CommandContextBinding AbiCommandInstance::bind(CommandContext& context) const {
    CommandContextBinding binding;
    binding.context = &context;
    binding.document = desc_.document;
    binding.transaction.transaction = context.transactionOrNull();
    // Left without an owner deliberately: the executor commits or aborts this
    // one, and a plugin must not be able to do it early.
    binding.transaction.owner = nullptr;
    return binding;
}

StepResult AbiCommandInstance::start(CommandContext& context) {
    if (desc_.onStart == nullptr) {
        return StepResult::failed("plugin command '" + desc_.name + "' has no start callback");
    }
    CommandContextBinding binding = bind(context);
    const CadStepResult result =
        desc_.onStart(reinterpret_cast<CadCommandContext*>(&binding), instance_);
    return toStepResult(result);
}

StepResult AbiCommandInstance::supply(const InputValue& input, CommandContext& context) {
    if (desc_.onSupply == nullptr) {
        return StepResult::failed("plugin command '" + desc_.name + "' has no supply callback");
    }
    CommandContextBinding binding = bind(context);
    std::vector<CadHandle> selection;
    const CadInputValue value = toCadInputValue(input, selection);
    const CadStepResult result =
        desc_.onSupply(reinterpret_cast<CadCommandContext*>(&binding), &value, instance_);
    return toStepResult(result);
}

void AbiCommandInstance::cleanup(CommandContext& context) {
    if (desc_.onCleanup == nullptr) { return; }
    CommandContextBinding binding = bind(context);
    desc_.onCleanup(reinterpret_cast<CadCommandContext*>(&binding), instance_);
}

}  // namespace cad::abi::detail
