#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cad/abi/cad_abi.h"
#include "cad/command/CommandExecutor.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Transaction.hpp"

/// The host side of the plugin boundary.
///
/// `cad_abi.h` is what a plugin author sees; this is what the application uses
/// to serve it. The split matters — the C header must stay compilable by a C89
/// compiler, a Rust `bindgen` run or a C# P/Invoke generator, so none of the
/// C++ below may appear in it.
namespace cad::abi {

/// The host side of an opaque `CadTransaction*`.
///
/// Carries the manager alongside the transaction, because commit and abort are
/// the manager's operations, not the transaction's. `owner` is deliberately
/// null for a transaction the *host* owns — a running command's — which is how
/// `commit_transaction` can refuse: the executor guarantees exactly one commit
/// or rollback per command, and a plugin committing early would break that
/// guarantee silently.
struct TransactionBinding {
    Transaction* transaction = nullptr;
    TransactionManager* owner = nullptr;
};

/// The drawing a plugin sees, behind an opaque `CadDocument*`.
///
/// A binding rather than an owner: the application already owns the database,
/// the undo stack and the executor, and duplicating that ownership here is how
/// two sources of truth get created.
class HostDocument {
public:
    /// @param executor may be null in headless contexts, which only disables
    ///   the calls that genuinely need one (`execute_command`, logging).
    HostDocument(Database& db, TransactionManager& transactions,
                 CommandExecutor* executor = nullptr) noexcept
        : db_(&db), transactions_(&transactions), executor_(executor) {}

    [[nodiscard]] Database& db() const noexcept { return *db_; }
    [[nodiscard]] TransactionManager& transactions() const noexcept { return *transactions_; }
    [[nodiscard]] CommandExecutor* executor() const noexcept { return executor_; }

    void setExecutor(CommandExecutor* executor) noexcept { executor_ = executor; }

    /// @internal Slot for a transaction a plugin opened through
    /// `begin_transaction`. One slot suffices because the manager permits only
    /// one open transaction, and it lives here rather than being heap-allocated
    /// per call so a plugin that forgets to commit leaks nothing.
    [[nodiscard]] TransactionBinding& pluginTransaction() noexcept { return pluginTransaction_; }

private:
    Database* db_;
    TransactionManager* transactions_;
    CommandExecutor* executor_;
    TransactionBinding pluginTransaction_;
};

/// One loaded plugin's registrations, behind an opaque `CadPluginHost*`.
///
/// Owns the registration tokens, so destroying the host removes every command
/// the plugin contributed. That is what makes unloading safe: the registry
/// cannot be left holding a function pointer into a DLL that is no longer
/// mapped.
class PluginHost {
public:
    PluginHost(std::string name, CommandRegistry& registry, HostDocument& document);
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;
    PluginHost(PluginHost&&) = delete;
    PluginHost& operator=(PluginHost&&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] CommandRegistry& registry() const noexcept { return *registry_; }
    [[nodiscard]] HostDocument& document() const noexcept { return *document_; }

    /// Register a command described across the C boundary.
    ///
    /// Every string in `desc` is copied, so the descriptor may be a stack
    /// local. The function pointers are retained and must outlive the
    /// registration.
    CadStatus registerCommand(const CadCommandDesc& desc);

    bool unregisterCommand(const std::string& id);

    /// Ids of the commands this plugin registered, in registration order.
    [[nodiscard]] std::vector<std::string> commandIds() const;

    [[nodiscard]] std::size_t commandCount() const noexcept { return commands_.size(); }

private:
    struct Registered;

    std::string name_;
    CommandRegistry* registry_;
    HostDocument* document_;
    std::vector<std::unique_ptr<Registered>> commands_;
};

/// The function table handed to every plugin.
///
/// A single immutable instance: it holds no state, only function pointers, so
/// there is nothing to make per-plugin.
[[nodiscard]] const CadApi& api() noexcept;

/// Whether a plugin reporting this ABI version can be served.
///
/// The major version must match exactly — a mismatch means the meaning of the
/// structs has changed and nothing can be assumed. A plugin's minor version may
/// be lower (it simply calls fewer functions) but not higher, since it would
/// call past the end of the table.
[[nodiscard]] constexpr bool isCompatibleVersion(std::uint32_t major,
                                                 std::uint32_t minor) noexcept {
    return major == CAD_ABI_VERSION_MAJOR && minor <= CAD_ABI_VERSION_MINOR;
}

}  // namespace cad::abi
