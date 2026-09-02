#include "config_io.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

#include "config_migrate.h"

namespace eqcore {

namespace {

// fsyncs a directory so a rename into it is durable. Best effort: some
// filesystems reject it, which is not a reason to fail the save.
void syncDirectory(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
}

bool writeFileDurably(const std::filesystem::path& path, const std::string& contents,
                       std::string& error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = std::string("open: ") + std::strerror(errno);
        return false;
    }

    const char* data = contents.data();
    std::size_t remaining = contents.size();
    while (remaining > 0) {
        const ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("write: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }

    if (::fsync(fd) != 0) {
        error = std::string("fsync: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    if (::close(fd) != 0) {
        error = std::string("close: ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

std::string configFilePath() {
    if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdgConfigHome) / "pipeeq" / "config.json";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".config" / "pipeeq" / "config.json";
}

LoadResult loadConfig() {
    LoadResult result;
    result.path = configFilePath();

    std::ifstream in(result.path);
    if (!in.is_open()) {
        result.status = LoadStatus::Missing;
        return result;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        result.status = LoadStatus::Failed;
        result.message = std::string("not valid JSON: ") + e.what();
        return result;
    }

    // Version detection has to be lenient about TYPE but strict about presence.
    //
    // Reading it with is_number_integer() only meant "version": 2.0 or "2" fell
    // through to 1, ran the v1 migration, found no "routes", and produced an
    // empty-but-VALID config that was then persisted straight over the original.
    // Every output, channel, send and curve gone. The presence of the key at
    // all is what marks a document as v2-or-newer.
    int version = 1;
    if (j.is_object()) {
        const auto it = j.find("version");
        if (it != j.end()) {
            if (it->is_number()) {
                version = static_cast<int>(it->get<double>());
            } else if (it->is_string()) {
                bool ok = false;
                const std::string text = it->get<std::string>();
                try {
                    version = std::stoi(text);
                    ok = true;
                } catch (const std::exception&) {
                    ok = false;
                }
                if (!ok) {
                    result.status = LoadStatus::Failed;
                    result.message = "\"version\" is not a number (" + text + ")";
                    return result;
                }
            } else {
                result.status = LoadStatus::Failed;
                result.message = "\"version\" is present but is not a number";
                return result;
            }
        }
    }
    if (version > kConfigVersion) {
        result.status = LoadStatus::Failed;
        result.message = "config version " + std::to_string(version) +
                          " was written by a newer PipeEQ (this build understands v" +
                          std::to_string(kConfigVersion) + ")";
        return result;
    }

    if (version < kConfigVersion) {
        auto migrated = migrateV1(j, result.warnings);
        if (!migrated) {
            result.status = LoadStatus::Failed;
            result.message = "could not migrate the v" + std::to_string(version) + " config";
            return result;
        }

        // Copy the original aside BEFORE anything can overwrite it. If the
        // backup can't be written, refuse the migration rather than proceed
        // with an unreversible one.
        const std::string backupPath = result.path + ".v" + std::to_string(version) + ".bak";
        std::error_code ec;
        std::filesystem::copy_file(result.path, backupPath,
                                    std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            result.status = LoadStatus::Failed;
            result.message = "refusing to migrate: could not back up the existing config to " +
                              backupPath + " (" + ec.message() + ")";
            return result;
        }

        // A migration that yields nothing from a document that plainly had
        // content is a misread, not an empty config. Persisting it would
        // destroy the original, which is exactly what persistable() exists to
        // prevent.
        if (migrated->outputs.empty() && j.is_object() && j.contains("routes")) {
            result.status = LoadStatus::Failed;
            result.message = "the v" + std::to_string(version) +
                              " config has a \"routes\" key but no output could be read from it";
            return result;
        }

        result.config = std::move(*migrated);
        result.status = LoadStatus::Migrated;
        result.message = "migrated from v" + std::to_string(version) + "; original saved as " + backupPath;
        return result;
    }

    auto parsed = parseV2(j, result.warnings);
    if (!parsed) {
        result.status = LoadStatus::Failed;
        result.message = "config is v" + std::to_string(kConfigVersion) + " but could not be read";
        return result;
    }

    result.config = std::move(*parsed);
    result.status = LoadStatus::Loaded;
    return result;
}

bool saveConfig(const AppConfig& config) {
    const std::filesystem::path path = configFilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        std::cerr << "pipeeq: cannot create " << path.parent_path() << ": " << ec.message() << "\n";
        return false;
    }

    std::ostringstream body;
    body << nlohmann::json(config).dump(2) << '\n';

    // Write-then-rename rather than truncating the real file in place: a crash
    // or full disk midway through would otherwise leave a half-written config.
    const std::filesystem::path tempPath = path.string() + ".tmp";
    std::string error;
    if (!writeFileDurably(tempPath, body.str(), error)) {
        std::cerr << "pipeeq: failed to write " << tempPath << ": " << error << "\n";
        std::error_code ignored;
        std::filesystem::remove(tempPath, ignored);
        return false;
    }

    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::cerr << "pipeeq: failed to replace " << path << ": " << ec.message() << "\n";
        std::error_code ignored;
        std::filesystem::remove(tempPath, ignored);
        return false;
    }

    syncDirectory(path.parent_path());
    return true;
}

} // namespace eqcore
