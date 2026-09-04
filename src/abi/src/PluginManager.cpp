#include "cad/abi/PluginManager.hpp"

#include <algorithm>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace cad::abi {
namespace {

// --------------------------------------------------------- platform loader
//
// Two implementations of three operations. Kept this small and this close
// together on purpose: the differences between the platforms are real but
// tiny, and hiding them behind an abstraction layer would be more code than
// the code it abstracts.

#if defined(_WIN32)

using LibraryHandle = HMODULE;

[[nodiscard]] LibraryHandle openLibrary(const std::filesystem::path& path, std::string& error) {
    // LOAD_WITH_ALTERED_SEARCH_PATH makes the plugin's own directory the first
    // place its dependencies are looked for, so a plugin may ship the DLLs it
    // needs beside itself instead of requiring them on PATH.
    const HMODULE handle =
        ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (handle == nullptr) {
        const DWORD code = ::GetLastError();
        error = std::system_category().message(static_cast<int>(code));
    }
    return handle;
}

[[nodiscard]] void* findSymbol(LibraryHandle library, const char* name) {
    // The cast is the documented way to use GetProcAddress and unavoidable:
    // the Windows API returns a function pointer type for what may be a data
    // symbol, and the standard has no conversion for that.
    return reinterpret_cast<void*>(::GetProcAddress(library, name));
}

void closeLibrary(LibraryHandle library) { ::FreeLibrary(library); }

#else

using LibraryHandle = void*;

[[nodiscard]] LibraryHandle openLibrary(const std::filesystem::path& path, std::string& error) {
    // RTLD_LOCAL so one plugin's symbols cannot satisfy another's imports,
    // which otherwise produces failures that depend on load order.
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* message = ::dlerror();
        error = message != nullptr ? message : "dlopen failed";
    }
    return handle;
}

[[nodiscard]] void* findSymbol(LibraryHandle library, const char* name) {
    return ::dlsym(library, name);
}

void closeLibrary(LibraryHandle library) { ::dlclose(library); }

#endif

template <typename Fn>
[[nodiscard]] Fn symbolAs(LibraryHandle library, const char* name) {
    return reinterpret_cast<Fn>(findSymbol(library, name));
}

}  // namespace

/// One loaded plugin.
///
/// Member order is load-bearing. Destruction runs in reverse declaration
/// order, so `host` — which unregisters the plugin's commands — is torn down
/// before `library` is unmapped. The other order leaves the registry holding
/// function pointers into freed address space, and the crash lands in whatever
/// unrelated code next opens the command palette.
struct PluginManager::Loaded {
    std::filesystem::path path;
    std::string name;
    std::uint32_t versionMajor = 0;
    std::uint32_t versionMinor = 0;
    LibraryHandle library = {};
    CadPluginShutdownFn shutdown = nullptr;
    std::unique_ptr<PluginHost> host;

    ~Loaded() {
        // Commands go first, then the plugin's own cleanup, then the library.
        host.reset();
        if (shutdown != nullptr) {
            try {
                shutdown();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // A plugin that throws out of shutdown has already failed; the
                // library still has to be unmapped.
            }
        }
        if (library != LibraryHandle{}) { closeLibrary(library); }
    }

    Loaded() = default;
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    Loaded(Loaded&&) = delete;
    Loaded& operator=(Loaded&&) = delete;
};

PluginManager::PluginManager(CommandRegistry& registry, HostDocument& document)
    : registry_(&registry), document_(&document) {}

PluginManager::~PluginManager() = default;

std::string_view PluginManager::librarySuffix() noexcept {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

PluginLoadResult PluginManager::load(const std::filesystem::path& path) {
    PluginLoadResult result;
    result.path = path;

    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path target = ec ? path : absolute;
    result.path = target;

    const bool alreadyLoaded =
        std::any_of(loaded_.begin(), loaded_.end(),
                    [&](const std::unique_ptr<Loaded>& entry) { return entry->path == target; });
    if (alreadyLoaded) {
        result.message = "already loaded";
        return result;
    }

    if (!std::filesystem::exists(target, ec)) {
        result.message = "file not found";
        return result;
    }

    auto entry = std::make_unique<Loaded>();
    entry->path = target;
    entry->name = target.stem().string();

    std::string error;
    entry->library = openLibrary(target, error);
    if (entry->library == LibraryHandle{}) {
        result.message = "could not load: " + error;
        return result;
    }

    // Version first, before anything of the plugin's runs. A plugin built
    // against an incompatible ABI would be passed structs it would misread, so
    // there is no safe way to ask it anything else.
    if (auto version = symbolAs<CadPluginVersionFn>(entry->library, CAD_PLUGIN_VERSION_SYMBOL)) {
        version(&entry->versionMajor, &entry->versionMinor);
        if (!isCompatibleVersion(entry->versionMajor, entry->versionMinor)) {
            result.message = "built for ABI " + std::to_string(entry->versionMajor) + "." +
                             std::to_string(entry->versionMinor) + "; this host provides " +
                             std::to_string(CAD_ABI_VERSION_MAJOR) + "." +
                             std::to_string(CAD_ABI_VERSION_MINOR);
            return result;
        }
    } else {
        // Optional, so its absence is not a failure — but it is the only way
        // to be refused safely rather than crashing, so record what we assumed.
        entry->versionMajor = CAD_ABI_VERSION_MAJOR;
        entry->versionMinor = CAD_ABI_VERSION_MINOR;
    }

    const auto init = symbolAs<CadPluginInitFn>(entry->library, CAD_PLUGIN_INIT_SYMBOL);
    if (init == nullptr) {
        result.message = "no " + std::string{CAD_PLUGIN_INIT_SYMBOL} + " entry point";
        return result;
    }
    entry->shutdown = symbolAs<CadPluginShutdownFn>(entry->library, CAD_PLUGIN_SHUTDOWN_SYMBOL);

    entry->host = std::make_unique<PluginHost>(entry->name, *registry_, *document_);

    std::int32_t status = CAD_ERROR_INTERNAL;
    try {
        status = init(&api(), reinterpret_cast<CadPluginHost*>(entry->host.get()));
    } catch (...) {
        // An exception should not be able to cross the boundary at all, but a
        // plugin compiled with a matching toolchain can in fact throw through
        // it. Refusing the plugin is better than letting it unwind further.
        result.message = "the plugin threw during initialisation";
        return result;
    }

    if (status != CAD_OK) {
        result.message = "initialisation failed with status " + std::to_string(status) + ": " +
                         api().last_error();
        return result;
    }

    result.loaded = true;
    result.message = std::to_string(entry->host->commandCount()) + " command(s)";
    loaded_.push_back(std::move(entry));
    return result;
}

std::vector<PluginLoadResult> PluginManager::loadDirectory(
    const std::filesystem::path& directory) {
    std::vector<PluginLoadResult> results;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) { return results; }

    std::vector<std::filesystem::path> candidates;
    for (const std::filesystem::directory_entry& file :
         std::filesystem::directory_iterator{directory, ec}) {
        if (!file.is_regular_file(ec)) { continue; }
        if (file.path().extension() == librarySuffix()) { candidates.push_back(file.path()); }
    }
    std::sort(candidates.begin(), candidates.end());

    results.reserve(candidates.size());
    for (const std::filesystem::path& candidate : candidates) {
        results.push_back(load(candidate));
    }
    return results;
}

bool PluginManager::unload(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path target = ec ? path : absolute;

    const auto it =
        std::find_if(loaded_.begin(), loaded_.end(),
                     [&](const std::unique_ptr<Loaded>& entry) { return entry->path == target; });
    if (it == loaded_.end()) { return false; }
    loaded_.erase(it);
    return true;
}

void PluginManager::unloadAll() { loaded_.clear(); }

std::vector<PluginInfo> PluginManager::plugins() const {
    std::vector<PluginInfo> infos;
    infos.reserve(loaded_.size());
    for (const std::unique_ptr<Loaded>& entry : loaded_) {
        PluginInfo info;
        info.path = entry->path;
        info.name = entry->name;
        info.versionMajor = entry->versionMajor;
        info.versionMinor = entry->versionMinor;
        info.commandIds = entry->host->commandIds();
        infos.push_back(std::move(info));
    }
    return infos;
}

}  // namespace cad::abi
