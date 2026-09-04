#pragma once

/// Shared internals of the plugin boundary.
///
/// Nothing here is public: a plugin sees `cad_abi.h`, and the application sees
/// `PluginHost.hpp`. This is the machinery in between.

#include <string>
#include <vector>

#include "cad/abi/PluginHost.hpp"

namespace cad::abi::detail {

// ------------------------------------------------------------------- errors

/// Message for the last failure on this thread.
///
/// Thread-local because a plugin may well call from a worker thread, and one
/// global slot would let two threads overwrite each other's diagnosis — which
/// turns an already-awkward debugging session into a misleading one.
[[nodiscard]] std::string& errorSlot() noexcept;

/// Record a failure and return its status, so call sites stay one line.
CadStatus fail(CadStatus status, std::string message);

/// Run `fn`, converting any escaping exception into a status code.
///
/// Every entry point from C goes through this. An exception unwinding through
/// a C frame is undefined behaviour, and in practice terminates the process —
/// the boundary has to be exception-tight or it is not a boundary.
template <typename F>
[[nodiscard]] CadStatus guard(F&& fn) noexcept {
    try {
        errorSlot().clear();
        return fn();
    } catch (const std::exception& error) {
        errorSlot() = error.what();
        return CAD_ERROR_INTERNAL;
    } catch (...) {
        errorSlot() = "unknown exception crossing the plugin boundary";
        return CAD_ERROR_INTERNAL;
    }
}

/// True when a struct the other side filled in is large enough to include a
/// field ending at `endOffset`.
///
/// This is what lets version 1.1 append a field without breaking a plugin
/// compiled against 1.0: the older plugin reports the smaller size, and the
/// host reads only as far as that size allows.
[[nodiscard]] constexpr bool includes(std::uint32_t structSize, std::size_t endOffset) noexcept {
    return static_cast<std::size_t>(structSize) >= endOffset;
}

/// `includes` for a named field, so call sites read as the question they ask.
#define CAD_ABI_HAS(structSize, Type, field) \
    (::cad::abi::detail::includes((structSize), offsetof(Type, field) + sizeof(Type::field)))

// -------------------------------------------------------------- conversions

[[nodiscard]] inline Vec3 toVec3(const CadPoint& p) noexcept { return Vec3{p.x, p.y, p.z}; }

[[nodiscard]] inline CadPoint toCadPoint(const Vec3& v) noexcept {
    return CadPoint{v.x, v.y, v.z};
}

[[nodiscard]] CadBox toCadBox(const Box3& box) noexcept;

[[nodiscard]] Color toColor(const CadColor& color) noexcept;
[[nodiscard]] CadColor toCadColor(const Color& color) noexcept;

/// Copy a UTF-8 string into a caller-supplied buffer.
///
/// The two-call pattern: `out_required` always reports the size needed
/// including the terminator, so a caller may pass a null buffer purely to size
/// one. A truncating copy is never performed — a silently shortened layer name
/// is worse than a clear failure.
[[nodiscard]] CadStatus copyString(std::string_view text, char* buffer, std::size_t capacity,
                                   std::size_t* outRequired);

/// Copy a handle list into a caller-supplied array, same pattern.
[[nodiscard]] CadStatus copyHandles(const std::vector<Handle>& handles, CadHandle* buffer,
                                    std::uint32_t capacity, std::uint32_t* outCount);

// ---------------------------------------------------------------- bindings

/// What an opaque `CadCommandContext*` really points at.
///
/// A wrapper rather than the bare `CommandContext*` because a plugin needs the
/// document too, and `CommandContext` deliberately exposes only the database —
/// it has no idea which host binding it belongs to.
///
/// Valid only for the duration of one callback; the plugin must not retain it.
struct CommandContextBinding {
    CommandContext* context = nullptr;
    HostDocument* document = nullptr;
    TransactionBinding transaction;
};

/// A command descriptor with every string owned by the host.
///
/// The plugin's descriptor may be a stack local, and its strings may be
/// temporaries; keeping raw pointers to either would be a dangling read on the
/// first invocation.
struct OwnedCommandDesc {
    std::string id;
    std::string name;
    std::string title;
    std::string description;
    std::string category;
    std::vector<std::string> aliases;

    CadCommandCreateFn onCreate = nullptr;
    CadCommandDestroyFn onDestroy = nullptr;
    CadCommandStartFn onStart = nullptr;
    CadCommandSupplyFn onSupply = nullptr;
    CadCommandCleanupFn onCleanup = nullptr;
    void* userData = nullptr;

    HostDocument* document = nullptr;
};

/// Adapts a command implemented in C to the core's command interface.
///
/// This class is the whole point of the ABI: once a plugin's callbacks are
/// wearing a `CommandInstance`, the executor, the undo system, the command
/// line, the menus and the palette cannot tell it from a built-in — which is
/// exactly the "first-class" requirement, achieved by having a single
/// implementation of "what a command is" rather than two.
class AbiCommandInstance final : public CommandInstance {
public:
    explicit AbiCommandInstance(const OwnedCommandDesc& desc);
    ~AbiCommandInstance() override;

    [[nodiscard]] StepResult start(CommandContext& context) override;
    [[nodiscard]] StepResult supply(const InputValue& input, CommandContext& context) override;
    void cleanup(CommandContext& context) override;

private:
    [[nodiscard]] CommandContextBinding bind(CommandContext& context) const;

    const OwnedCommandDesc& desc_;
    /// Per-invocation state the plugin allocated, or the descriptor's
    /// `user_data` when it declared no create hook.
    void* instance_ = nullptr;
    bool ownsInstance_ = false;
};

/// Translate a step result returned from C into the core's form, copying every
/// string out of the plugin's memory before it can go away.
[[nodiscard]] StepResult toStepResult(const CadStepResult& result);

/// Translate an input value into the C form.
///
/// The returned struct borrows `input`'s strings and `scratch`, which the
/// caller must keep alive for the duration of the callback. `scratch` exists
/// rather than reinterpreting the `Handle` vector in place: the two are
/// layout-compatible in practice, but borrowing a plain array costs one copy
/// of a selection and avoids resting on that.
[[nodiscard]] CadInputValue toCadInputValue(const InputValue& input,
                                            std::vector<CadHandle>& scratch);

}  // namespace cad::abi::detail
