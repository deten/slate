#include "cad/io/OdaFileConverter.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace cad::io {
namespace {

/// Run a program to completion, or kill it when the timeout expires.
///
/// The timeout is the point of this function. The converter is a Qt GUI
/// application run headless, and on a machine without a usable display it can
/// block indefinitely instead of exiting — which, without this, hangs the whole
/// application on File > Open with no way out.
///
/// Returns the exit code, or -1 when the process could not be started.
/// @throws TranslationError on timeout.
[[nodiscard]] int runToCompletion(const std::filesystem::path& executable,
                                  const std::vector<std::string>& arguments,
                                  std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    // Windows takes one command line, not an argument vector, so the arguments
    // are quoted back together. Paths here are temporary directories we made,
    // but quoting is not optional — the *source* file's name reaches this via
    // the staged copy's directory, and drawings live in folders with spaces.
    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    for (const std::string& argument : arguments) {
        const std::wstring wide(argument.begin(), argument.end());
        commandLine += L" \"" + wide + L"\"";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    const BOOL started = ::CreateProcessW(
        executable.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (started == FALSE) {
        return -1;
    }

    const DWORD waited =
        ::WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count()));
    if (waited == WAIT_TIMEOUT) {
        ::TerminateProcess(process.hProcess, 1);
        ::CloseHandle(process.hProcess);
        ::CloseHandle(process.hThread);
        throw TranslationError(
            TranslationFailure::Timeout,
            "The converter did not finish within " + std::to_string(timeout.count() / 1000) +
                " seconds and was stopped.",
            "It may be waiting for a display. Try converting the file to DXF by hand, "
            "or open it in other CAD software and save it as DXF.");
    }

    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hProcess);
    ::CloseHandle(process.hThread);
    return static_cast<int>(exitCode);
#else
    std::vector<char*> argv;
    std::string program = executable.string();
    argv.push_back(program.data());
    std::vector<std::string> owned = arguments;
    for (std::string& argument : owned) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    if (::posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }

    // Polled rather than blocking, because waitpid has no timeout and the
    // reason this function exists is that the converter can fail to exit.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        const pid_t finished = ::waitpid(pid, &status, WNOHANG);
        if (finished == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (finished < 0) {
            return -1;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            throw TranslationError(
                TranslationFailure::Timeout,
                "The converter did not finish within " +
                    std::to_string(timeout.count() / 1000) + " seconds and was stopped.",
                "It may be waiting for a display.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
#endif
}

[[nodiscard]] bool isFile(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

/// The converter's executable name on this platform.
[[nodiscard]] std::filesystem::path executableName() {
#if defined(_WIN32)
    return "ODAFileConverter.exe";
#else
    return "ODAFileConverter";
#endif
}

/// Add every `<parent>/<anything>/ODAFileConverter` under `parent`.
///
/// The Windows installer creates a *version-named* directory —
/// `ODAFileConverter 26.4.0` — so a fixed path list goes stale with every
/// release. Enumerating is the only approach that keeps working.
void appendVersionedInstalls(const std::filesystem::path& parent,
                             std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) {
        return;
    }
    std::vector<std::filesystem::path> found;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{parent, ec}) {
        if (entry.is_directory(ec)) {
            found.push_back(entry.path() / executableName());
        }
    }
    // Newest last by name, so the highest version is preferred.
    std::sort(found.begin(), found.end());
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
        out.push_back(*it);
    }
}

/// Directory of the running executable, for a converter shipped alongside.
[[nodiscard]] std::filesystem::path applicationDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path{buffer}.parent_path();
#else
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : self.parent_path();
#endif
}

[[nodiscard]] std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw TranslationError(TranslationFailure::TranslatorFailed,
                               "Could not read the converter's output.");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// The converter's own explanation, when it left one.
///
/// On a failure it writes `<name>.err` beside where the output would have gone
/// and says nothing on stdout. Reading it turns "produced no output" into the
/// actual reason, which is the difference between a usable error and a
/// shrug — and it is how the writer's own bugs get found.
[[nodiscard]] std::string readErrorReport(const std::filesystem::path& directory) {
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{directory, ec}) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".err") {
            continue;
        }
        std::ifstream input{entry.path()};
        std::ostringstream buffer;
        buffer << input.rdbuf();
        std::string text = buffer.str();
        // Collapsed to one line: this ends up in a dialog and a log line.
        for (char& c : text) {
            if (c == '\r' || c == '\n') {
                c = ' ';
            }
        }
        // Trim the run of spaces the collapse leaves behind.
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        return text;
    }
    return {};
}

/// The first file in `directory` with the given extension.
[[nodiscard]] std::filesystem::path findProduced(const std::filesystem::path& directory,
                                                 std::string_view extension) {
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{directory, ec}) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string suffix = entry.path().extension().string();
        for (char& c : suffix) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (suffix == extension) {
            return entry.path();
        }
    }
    return {};
}

/// A private directory for one conversion, removed however this scope exits.
///
/// It holds a full copy of the user's drawing, so leaving it behind on a
/// failure path would scatter copies of confidential work through the temp
/// directory.
class ScratchDirectory {
public:
    ScratchDirectory() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            throw TranslationError(TranslationFailure::TranslatorFailed,
                                   "No temporary directory is available.");
        }
        // A counter rather than a random name: this is not a security boundary,
        // and a collision simply moves to the next number.
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = base / ("slate-dwg-" + std::to_string(stamp) + "-" +
                            std::to_string(attempt));
            if (std::filesystem::create_directories(root_, ec)) {
                std::filesystem::create_directories(root_ / "in", ec);
                std::filesystem::create_directories(root_ / "out", ec);
                return;
            }
        }
        throw TranslationError(TranslationFailure::TranslatorFailed,
                               "Could not create a working directory for the conversion.");
    }

    ~ScratchDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;

    [[nodiscard]] std::filesystem::path in() const { return root_ / "in"; }
    [[nodiscard]] std::filesystem::path out() const { return root_ / "out"; }

private:
    std::filesystem::path root_;
};

constexpr const char* kDownloadRemedy =
    "Install the free ODA File Converter from "
    "https://www.opendesign.com/guestfiles/oda_file_converter, or set the "
    "CAD_ODA_CONVERTER environment variable to its executable. Alternatively, open "
    "the drawing in other CAD software and save it as DXF.";

}  // namespace

OdaFileConverter::OdaFileConverter(std::filesystem::path explicitPath)
    : explicitPath_(std::move(explicitPath)) {}

std::vector<std::filesystem::path> OdaFileConverter::searchLocations() {
    std::vector<std::filesystem::path> locations;

    // The environment variable wins, so a test or a portable install needs no
    // code change and no registry.
    if (const char* fromEnvironment = std::getenv(kPathEnvironmentVariable);
        fromEnvironment != nullptr && *fromEnvironment != '\0') {
        locations.emplace_back(fromEnvironment);
    }

    // Shipped alongside the application keeps the portable build portable.
    if (const std::filesystem::path appDir = applicationDirectory(); !appDir.empty()) {
        locations.push_back(appDir / "converters" / "ODAFileConverter" / executableName());
        appendVersionedInstalls(appDir / "converters", locations);
    }

#if defined(_WIN32)
    // The installer writes no registry entry and puts nothing on PATH, so the
    // well-known locations have to be walked.
    appendVersionedInstalls("C:/Program Files/ODA", locations);
    appendVersionedInstalls("C:/Program Files (x86)/ODA", locations);
    locations.emplace_back("C:/Program Files/ODA/ODAFileConverter/ODAFileConverter.exe");
    locations.emplace_back("C:/Program Files (x86)/ODA/ODAFileConverter/ODAFileConverter.exe");
#elif defined(__APPLE__)
    locations.emplace_back("/Applications/ODAFileConverter.app/Contents/MacOS/ODAFileConverter");
#else
    locations.emplace_back("/usr/bin/ODAFileConverter");
    locations.emplace_back("/usr/local/bin/ODAFileConverter");
    appendVersionedInstalls("/opt", locations);
#endif

    return locations;
}

std::filesystem::path OdaFileConverter::findExecutable() const {
    if (!explicitPath_.empty()) {
        // An explicit path is a statement of intent: if it is wrong, say so
        // rather than quietly searching elsewhere and using something else.
        return isFile(explicitPath_) ? explicitPath_ : std::filesystem::path{};
    }
    for (const std::filesystem::path& candidate : searchLocations()) {
        if (isFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

DwgTranslator::Status OdaFileConverter::probe() const {
    Status status;
    const std::filesystem::path executable = findExecutable();
    if (!executable.empty()) {
        status.available = true;
        return status;
    }

    status.available = false;
    if (!explicitPath_.empty()) {
        status.problem = "No converter at " + explicitPath_.string() + ".";
    } else {
        status.problem = "The ODA File Converter was not found.";
    }
    status.remedy = kDownloadRemedy;
    return status;
}

void DwgTranslator::writeDwg(const std::string& /*dxfText*/,
                             const std::filesystem::path& /*target*/) const {
    throw TranslationError(TranslationFailure::TranslatorUnavailable,
                           displayName() + " reads DWG but does not write it.",
                           "Save the drawing as DXF instead.");
}

std::string OdaFileConverter::toDxf(const std::filesystem::path& source) const {
    const std::filesystem::path executable = findExecutable();
    if (executable.empty()) {
        const Status status = probe();
        throw TranslationError(TranslationFailure::TranslatorUnavailable, status.problem,
                               status.remedy);
    }
    if (!isFile(source)) {
        throw TranslationError(TranslationFailure::SourceUnreadable,
                               "Cannot read " + source.string() + ".",
                               "Check that the file exists and that you can read it.");
    }

    ScratchDirectory scratch;

    // Staged under a name with no spaces or punctuation. The converter takes a
    // filter argument and quotes paths inconsistently across platforms; a plain
    // name removes a whole class of failure that would otherwise depend on what
    // the user called their drawing.
    const std::filesystem::path staged = scratch.in() / "drawing.dwg";
    std::error_code ec;
    std::filesystem::copy_file(source, staged, std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        throw TranslationError(TranslationFailure::SourceUnreadable,
                               "Could not stage " + source.filename().string() +
                                   " for conversion: " + ec.message(),
                               "Check that the file is not open in another application.");
    }

    if (onProgress_) {
        onProgress_("Converting " + source.filename().string() + " with " + displayName() + "…");
    }

    const int exitCode = runToCompletion(executable,
                                         {
                                             scratch.in().string(),
                                             scratch.out().string(),
                                             // Newest version the tool emits;
                                             // it loses the least, and the
                                             // reader accepts every version.
                                             "ACAD2018",
                                             // ASCII DXF, which is what the
                                             // reader consumes.
                                             "DXF",
                                             // No recursion: the staging
                                             // directory is flat by design.
                                             "0",
                                             // Audit on. It repairs common
                                             // corruption rather than refusing.
                                             "1",
                                         },
                                         timeout_);

    if (exitCode < 0) {
        throw TranslationError(TranslationFailure::TranslatorFailed,
                               "Could not run " + executable.string() + ".",
                               "Check that the ODA File Converter is installed correctly.");
    }

    const std::filesystem::path produced = findProduced(scratch.out(), ".dxf");
    if (produced.empty()) {
        // The real success test. The converter exits 0 having written nothing
        // for several classes of failure, so trusting the exit code here would
        // report success and then fail confusingly further along.
        const std::string reported = readErrorReport(scratch.out());
        throw TranslationError(
            TranslationFailure::UnsupportedFormat,
            displayName() + " produced no output for " + source.filename().string() + "." +
                (reported.empty() ? "" : " " + reported),
            "The file may be corrupt, password-protected, or a DWG version this "
            "converter does not support. Try opening it in other CAD software and saving to DXF.");
    }

    return readTextFile(produced);
}

void OdaFileConverter::fromDxf(const std::string& dxfText, const std::filesystem::path& target,
                               std::string_view version) const {
    const std::filesystem::path executable = findExecutable();
    if (executable.empty()) {
        const Status status = probe();
        throw TranslationError(TranslationFailure::TranslatorUnavailable, status.problem,
                               status.remedy);
    }

    ScratchDirectory scratch;
    const std::filesystem::path staged = scratch.in() / "drawing.dxf";
    {
        std::ofstream output{staged, std::ios::binary};
        if (!output) {
            throw TranslationError(TranslationFailure::TranslatorFailed,
                                   "Could not stage the drawing for conversion.");
        }
        output.write(dxfText.data(), static_cast<std::streamsize>(dxfText.size()));
    }

    const int exitCode = runToCompletion(
        executable,
        {scratch.in().string(), scratch.out().string(), std::string{version}, "DWG", "0", "1"},
        timeout_);
    if (exitCode < 0) {
        throw TranslationError(TranslationFailure::TranslatorFailed,
                               "Could not run " + executable.string() + ".");
    }

    const std::filesystem::path produced = findProduced(scratch.out(), ".dwg");
    if (produced.empty()) {
        const std::string reported = readErrorReport(scratch.out());
        throw TranslationError(
            TranslationFailure::TranslatorFailed,
            "The converter produced no DWG output." +
                (reported.empty() ? std::string{} : " " + reported),
            "Check that the drawing contains no unsupported content.");
    }

    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::copy_file(produced, target,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw TranslationError(TranslationFailure::TranslatorFailed,
                               "Could not write " + target.string() + ": " + ec.message());
    }
}

std::unique_ptr<DwgTranslator> makeDwgTranslator() {
    return std::make_unique<OdaFileConverter>();
}

}  // namespace cad::io
