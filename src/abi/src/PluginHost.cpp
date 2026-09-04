#include "cad/abi/PluginHost.hpp"

#include <algorithm>
#include <stdexcept>

#include "AbiInternal.hpp"

namespace cad::abi {

/// One command a plugin contributed.
///
/// The owned descriptor and the registration token are kept together so that
/// dropping this struct both removes the command from the registry and frees
/// the strings it referenced — in that order, which matters: the registry must
/// stop being able to construct instances before the descriptor they point at
/// goes away.
struct PluginHost::Registered {
    std::unique_ptr<detail::OwnedCommandDesc> desc;
    CommandRegistration registration;

    ~Registered() { registration.reset(); }
};

PluginHost::PluginHost(std::string name, CommandRegistry& registry, HostDocument& document)
    : name_(std::move(name)), registry_(&registry), document_(&document) {}

PluginHost::~PluginHost() = default;

namespace {

/// Read a possibly-null C string into an owned one.
[[nodiscard]] std::string own(const char* text) {
    return text != nullptr ? std::string{text} : std::string{};
}

[[nodiscard]] CommandTransactionMode toTransactionMode(std::int32_t mode) noexcept {
    // `Manual` is deliberately not reachable from the ABI. A plugin managing
    // its own transactions could commit halfway through its own command and
    // leave the undo stack describing a state the drawing was never in; the
    // built-in UNDO command needs that power, third-party code does not.
    return mode == CAD_TRANSACTION_READ_ONLY ? CommandTransactionMode::ReadOnly
                                             : CommandTransactionMode::Single;
}

}  // namespace

CadStatus PluginHost::registerCommand(const CadCommandDesc& source) {
    using detail::fail;

    const std::uint32_t size = source.struct_size;
    if (size == 0) {
        return fail(CAD_ERROR_INVALID_ARGUMENT,
                    "CadCommandDesc.struct_size must be set; it is how the host knows which "
                    "fields the plugin filled in");
    }
    if (!CAD_ABI_HAS(size, CadCommandDesc, on_supply)) {
        return fail(CAD_ERROR_INVALID_ARGUMENT,
                    "CadCommandDesc is too small to contain the required callbacks");
    }
    if (source.id == nullptr || *source.id == '\0') {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "a command needs a stable id");
    }
    if (source.name == nullptr || *source.name == '\0') {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "a command needs a name to type");
    }
    if (source.on_start == nullptr || source.on_supply == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT,
                    "a command needs both on_start and on_supply; a command that finishes "
                    "immediately still returns CAD_STEP_DONE from on_start");
    }

    auto owned = std::make_unique<detail::OwnedCommandDesc>();
    owned->id = own(source.id);
    owned->name = own(source.name);
    owned->title = own(source.title);
    owned->description = own(source.description);
    owned->category = own(source.category);
    if (owned->title.empty()) { owned->title = owned->name; }
    if (owned->category.empty()) {
        // Uncategorized commands would vanish from the generated menus, which
        // looks like the plugin failed to load. A named fallback keeps it
        // visible and makes its origin obvious.
        owned->category = "Plugins";
    }

    if (CAD_ABI_HAS(size, CadCommandDesc, alias_count) && source.aliases != nullptr) {
        owned->aliases.reserve(source.alias_count);
        for (std::uint32_t i = 0; i < source.alias_count; ++i) {
            if (source.aliases[i] != nullptr && *source.aliases[i] != '\0') {
                owned->aliases.emplace_back(source.aliases[i]);
            }
        }
    }

    owned->onCreate = source.on_create;
    owned->onDestroy = source.on_destroy;
    owned->onStart = source.on_start;
    owned->onSupply = source.on_supply;
    owned->onCleanup =
        CAD_ABI_HAS(size, CadCommandDesc, on_cleanup) ? source.on_cleanup : nullptr;
    owned->userData = CAD_ABI_HAS(size, CadCommandDesc, user_data) ? source.user_data : nullptr;
    owned->document = document_;

    CommandDefinition definition;
    definition.id = owned->id;
    definition.name = owned->name;
    definition.aliases = owned->aliases;
    definition.title = owned->title;
    definition.description = owned->description;
    definition.category = owned->category;
    definition.fromPlugin = true;
    definition.transactionMode =
        CAD_ABI_HAS(size, CadCommandDesc, transaction_mode)
            ? toTransactionMode(source.transaction_mode)
            : CommandTransactionMode::Single;
    definition.supportsPickfirst = CAD_ABI_HAS(size, CadCommandDesc, supports_pickfirst) &&
                                   source.supports_pickfirst != 0;
    definition.repeatable =
        !CAD_ABI_HAS(size, CadCommandDesc, repeatable) || source.repeatable != 0;

    // The factory captures a raw pointer into `owned`, which this host keeps
    // alive for exactly as long as the registration. Capturing by value would
    // copy the strings once per invocation for no benefit.
    const detail::OwnedCommandDesc* descriptor = owned.get();
    definition.create = [descriptor] {
        return std::make_unique<detail::AbiCommandInstance>(*descriptor);
    };

    auto entry = std::make_unique<Registered>();
    entry->desc = std::move(owned);
    try {
        entry->registration = registry_->registerCommand(std::move(definition));
    } catch (const std::invalid_argument& error) {
        return fail(CAD_ERROR_ALREADY_EXISTS, error.what());
    }

    commands_.push_back(std::move(entry));
    return CAD_OK;
}

bool PluginHost::unregisterCommand(const std::string& id) {
    const auto it = std::find_if(commands_.begin(), commands_.end(),
                                 [&](const std::unique_ptr<Registered>& entry) {
                                     return entry->desc->id == id;
                                 });
    if (it == commands_.end()) { return false; }
    commands_.erase(it);
    return true;
}

std::vector<std::string> PluginHost::commandIds() const {
    std::vector<std::string> ids;
    ids.reserve(commands_.size());
    for (const std::unique_ptr<Registered>& entry : commands_) { ids.push_back(entry->desc->id); }
    return ids;
}

}  // namespace cad::abi
