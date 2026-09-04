/// Tests for auto-save and crash recovery.
///
/// The rule this code lives by is that it must never make things worse. It
/// writes beside the user's drawing rather than over it, and a recovery file is
/// only discarded once the real work is safe. Most of what is checked here is
/// that failure modes stay harmless.

#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "cad/io/RecoveryStore.hpp"

using namespace cad;
using namespace cad::io;

namespace {

/// A scratch directory that cleans up after itself even when a check fails.
struct Scratch {
    std::filesystem::path path;

    Scratch()
        : path(std::filesystem::temp_directory_path() /
               ("slate_recovery_" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path);
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

[[nodiscard]] std::string readAll(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// Nothing is running, which is the state after a crash.
[[nodiscard]] bool nothingAlive(std::uint64_t) {
    return false;
}

}  // namespace

TEST_SUITE("Recovery") {
    TEST_CASE("an auto-save writes the drawing and its metadata") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};

        REQUIRE(store.write("C:/drawings/Plan.dwg", "0\nSECTION\n"));

        const std::filesystem::path file = store.pathFor("C:/drawings/Plan.dwg");
        REQUIRE(std::filesystem::exists(file));
        CHECK(readAll(file) == "0\nSECTION\n");
        // Named after the drawing so the user can recognise it, and after the
        // session so two windows cannot collide.
        CHECK(file.filename().string().find("Plan") != std::string::npos);
        CHECK(file.filename().string().find("1234") != std::string::npos);
    }

    TEST_CASE("pending reports a crashed session's work with enough to judge it") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};
        REQUIRE(store.write("C:/drawings/Plan.dwg", "drawing"));

        const std::vector<RecoveryEntry> pending = store.pending(nothingAlive);
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].originalPath == "C:/drawings/Plan.dwg");
        CHECK(pending[0].processId == 1234);
        CHECK(pending[0].displayName() == "Plan.dwg");
        // A timestamp, so the user can see how much work the file represents
        // before deciding.
        CHECK(pending[0].savedAt.size() == 19);
    }

    TEST_CASE("a live session's auto-save is not offered as recoverable") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};
        REQUIRE(store.write("C:/drawings/Plan.dwg", "drawing"));

        // Another window has this drawing open right now. Offering to "recover"
        // it would invite the user to fork their own live work.
        const auto everythingAlive = [](std::uint64_t) { return true; };
        CHECK(store.pending(everythingAlive).empty());
    }

    TEST_CASE("an interrupted write leaves the previous auto-save intact") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};
        REQUIRE(store.write("Plan.dxf", "first version"));

        // Simulate the crash: a part-written temporary is left behind, as it
        // would be if the process died mid-write.
        std::filesystem::path partial = store.pathFor("Plan.dxf");
        partial += ".part";
        std::ofstream{partial, std::ios::binary} << "half a dr";

        // The good file is untouched, and the debris is not mistaken for one.
        CHECK(readAll(store.pathFor("Plan.dxf")) == "first version");
        const std::vector<RecoveryEntry> pending = store.pending(nothingAlive);
        REQUIRE(pending.size() == 1);
        CHECK(readAll(pending[0].recoveryFile) == "first version");
    }

    TEST_CASE("a second auto-save replaces the first rather than accumulating") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};
        REQUIRE(store.write("Plan.dxf", "first"));
        REQUIRE(store.write("Plan.dxf", "second"));

        const std::vector<RecoveryEntry> pending = store.pending(nothingAlive);
        REQUIRE(pending.size() == 1);
        CHECK(readAll(pending[0].recoveryFile) == "second");
    }

    TEST_CASE("discarding removes the drawing and its metadata together") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};
        REQUIRE(store.write("Plan.dxf", "drawing"));

        CHECK(store.discard("Plan.dxf"));
        CHECK(store.pending(nothingAlive).empty());
        // A stale sidecar left behind would make the next scan report a
        // recovery whose drawing no longer exists.
        CHECK_FALSE(std::filesystem::exists(store.pathFor("Plan.dxf")));
    }

    TEST_CASE("discardAll clears this session's files and leaves others alone") {
        Scratch scratch;
        RecoveryStore mine{scratch.path, 1234};
        RecoveryStore theirs{scratch.path, 5678};
        REQUIRE(mine.write("A.dxf", "mine"));
        REQUIRE(theirs.write("B.dxf", "theirs"));

        mine.discardAll();

        const std::vector<RecoveryEntry> pending = mine.pending(nothingAlive);
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].processId == 5678);
    }

    TEST_CASE("two sessions editing the same drawing do not overwrite each other") {
        Scratch scratch;
        RecoveryStore first{scratch.path, 1};
        RecoveryStore second{scratch.path, 2};
        REQUIRE(first.write("Plan.dxf", "from the first window"));
        REQUIRE(second.write("Plan.dxf", "from the second window"));

        CHECK(first.pathFor("Plan.dxf") != second.pathFor("Plan.dxf"));
        CHECK(first.pending(nothingAlive).size() == 2);
    }

    TEST_CASE("an unsaved drawing is still protected") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};
        REQUIRE(store.write("", "never saved anywhere"));

        const std::vector<RecoveryEntry> pending = store.pending(nothingAlive);
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].originalPath.empty());
        // The work with no filename is exactly the work a crash hurts most.
        CHECK(pending[0].displayName() == "Unsaved drawing");
        CHECK(readAll(pending[0].recoveryFile) == "never saved anywhere");
    }

    TEST_CASE("a drawing name that is not a safe filename cannot escape the folder") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};

        const std::filesystem::path path = store.pathFor("C:/x/../../Section A:B*?.dwg");
        // Whatever the drawing is called, the auto-save lands in the recovery
        // directory and nowhere else.
        CHECK(path.parent_path() == scratch.path);
        const std::string name = path.filename().string();
        CHECK(name.find(':') == std::string::npos);
        CHECK(name.find('*') == std::string::npos);
        CHECK(name.find('?') == std::string::npos);
    }

    TEST_CASE("scanning a directory that does not exist reports nothing") {
        const RecoveryStore store{std::filesystem::temp_directory_path() / "no-such-recovery-dir",
                                  1234};
        CHECK(store.pending(nothingAlive).empty());
    }

    TEST_CASE("remove takes a recovery entry out of circulation") {
        Scratch scratch;
        RecoveryStore store{scratch.path, 1234};
        REQUIRE(store.write("Plan.dxf", "drawing"));

        const std::vector<RecoveryEntry> pending = store.pending(nothingAlive);
        REQUIRE(pending.size() == 1);
        CHECK(RecoveryStore::remove(pending[0]));
        CHECK(store.pending(nothingAlive).empty());
    }
}
