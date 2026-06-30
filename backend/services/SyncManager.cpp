#include "SyncManager.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <random>
#include <functional>
#include <algorithm>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace {

// Simple, dependency-free aggregate hash: combine per-file (relative path,
// size, mtime) into a single running hash via std::hash + boost-style
// combine. This is an integrity *signal*, not a cryptographic guarantee —
// good enough to detect "something in nas_storage changed since last check"
// without pulling in a SHA library purely for this. If stronger guarantees
// are needed later (e.g. detecting content changes at identical size/mtime),
// swap this for a real content hash — the call site (checkHash()) doesn't
// care how the hash is computed, only that it's stable and deterministic
// for unchanged content.
void hashCombine(size_t &seed, size_t v) {
    seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

} // namespace

SyncManager &SyncManager::instance() {
    static SyncManager inst;
    return inst;
}

SyncManager::~SyncManager() { shutdown(); }

std::string SyncManager::nowIso8601Utc() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string SyncManager::computeStorageHash(const std::string &root) {
    size_t seed = 0;
    std::error_code ec;

    if (!fs::exists(root, ec)) return "";

    // Sorted-by-path iteration isn't guaranteed by directory_iterator, so
    // collect then sort — otherwise the same directory contents could
    // produce a different combined hash across runs purely from filesystem
    // iteration order, which would look like spurious drift.
    std::vector<fs::path> entries;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) continue;
        // .nas-meta holds events.db/alerts.db, which mutate on every action
        // (including the hash check's own log write) — including it would
        // make the storage hash non-deterministic across consecutive checks
        // for reasons that have nothing to do with user file content.
        if (it->path().filename() == ".nas-meta") {
            it.disable_recursion_pending();
            continue;
        }
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());

    for (auto &p : entries) {
        std::string rel = fs::relative(p, root, ec).string();
        hashCombine(seed, std::hash<std::string>{}(rel));

        if (fs::is_regular_file(p, ec)) {
            auto size = fs::file_size(p, ec);
            hashCombine(seed, std::hash<uintmax_t>{}(size));
            auto mtime = fs::last_write_time(p, ec);
            hashCombine(seed, std::hash<int64_t>{}(
                mtime.time_since_epoch().count()));
        }
    }

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << seed;
    return oss.str();
}

void SyncManager::init(const std::string &nasRoot,
                        int hashIntervalSeconds,
                        int portRotationIntervalSeconds,
                        const std::string &portalDir) {
    nasRoot_ = nasRoot;
    portalDir_ = portalDir;
    hashIntervalSeconds_ = hashIntervalSeconds;
    portRotationIntervalSeconds_ = portRotationIntervalSeconds;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status_.state = SyncState::Idle;
        status_.lastHash = computeStorageHash(nasRoot_);
        status_.lastHashCheckUtc = nowIso8601Utc();
    }
    lastHashCheckEpoch_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    log("info", "Sync manager initialized, baseline hash computed");

    // Rotate immediately on startup so currentPort is never 0/unset.
    maybeRotatePort();

    running_ = true;
    workerThread_ = std::thread(&SyncManager::runLoop, this);
}

void SyncManager::shutdown() {
    if (!running_) return;
    running_ = false;
    if (workerThread_.joinable()) workerThread_.join();
    stopPortalListener();
}

void SyncManager::runLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(kLoopTickSeconds));
        if (!running_) break;

        checkHash();
        maybeRotatePort();
        advanceMockSync();
    }
}

void SyncManager::checkHash() {
    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (nowEpoch - lastHashCheckEpoch_ < hashIntervalSeconds_) return;
    lastHashCheckEpoch_ = nowEpoch;

    std::string newHash = computeStorageHash(nasRoot_);

    bool enteredMismatch = false;
    bool syncCompletedHash = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);

        bool syncWasActive = (status_.state == SyncState::Syncing);
        bool changed = !status_.lastHash.empty() && newHash != status_.lastHash;

        status_.lastHash = newHash;
        status_.lastHashCheckUtc = nowIso8601Utc();

        if (changed && !syncWasActive && status_.state != SyncState::HashMismatch) {
            // Storage content changed but the sync engine wasn't running —
            // exactly the "hash changed for no reason we know about" case
            // from the design. This is the one state checkHash() can enter
            // on its own; Error is reserved for sync-engine-reported
            // failures (see advanceMockSync()).
            status_.state = SyncState::HashMismatch;
            enteredMismatch = true;
        } else if (changed && syncWasActive) {
            syncCompletedHash = true;
        }
    } // lock released before logging — log() takes the same mutex

    if (enteredMismatch) {
        log("error", "Storage hash changed with no sync in progress — "
                      "possible unverified modification outside HomeNAS");
    } else if (syncCompletedHash) {
        log("info", "Storage hash updated following sync operation");
    }
}

void SyncManager::maybeRotatePort() {
    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (lastPortRotationEpoch_ != 0 &&
        nowEpoch - lastPortRotationEpoch_ < portRotationIntervalSeconds_) return;
    lastPortRotationEpoch_ = nowEpoch;

    // Random port in the dynamic/private range, away from the main 8080
    // listener and common service ports.
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(49152, 65535);
    int newPort = dist(rng);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status_.currentPort = newPort;
        status_.portRotatedAtUtc = nowIso8601Utc();
    }
    log("info", "Sync service port rotated to " + std::to_string(newPort));

    // Stopgap: rebind the placeholder static-file listener to the new port.
    // See class-level doc comment in SyncManager.h — this goes away once a
    // real sync engine owns the rotating port itself.
    startPortalListener(newPort);
}

// ── stopgap portal listener ───────────────────────────────────────────────────

void SyncManager::startPortalListener(int port) {
    stopPortalListener();

    if (portalDir_.empty()) return; // listener disabled (see init() doc)
    if (!fs::exists(portalDir_)) {
        log("warning", "Portal directory not found, skipping listener spawn: "
                        + portalDir_);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log("error", "Failed to fork portal listener process");
        return;
    }

    if (pid == 0) {
        // Child: detach into its own session so it survives independently
        // of the parent's process group, then exec a minimal static file
        // server. Stdout/stderr redirected to /dev/null — this is a
        // placeholder utility process, not something whose output belongs
        // in the main backend's logs.
        setsid();
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        std::string portStr = std::to_string(port);
        execlp("python3", "python3", "-m", "http.server", portStr.c_str(),
               "--bind", "0.0.0.0", "--directory", portalDir_.c_str(),
               (char *)nullptr);

        // execlp only returns on failure
        _exit(127);
    }

    // Parent
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        portalListenerPid_ = pid;
    }
    log("info", "Portal listener started on port " + std::to_string(port)
                 + " (pid " + std::to_string(pid) + ")");
}

void SyncManager::stopPortalListener() {
    pid_t pid;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pid = portalListenerPid_;
        portalListenerPid_ = -1;
    }
    if (pid <= 0) return;

    kill(pid, SIGTERM);
    // Reap without blocking the caller indefinitely — WNOHANG means a
    // slow-to-exit child won't stall rotation or shutdown; it'll just
    // become a brief zombie reaped on the next rotation/shutdown call or
    // by init's default handling. Acceptable for a once-a-day stopgap.
    int status = 0;
    waitpid(pid, &status, WNOHANG);
}

void SyncManager::advanceMockSync() {
    bool completed = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (status_.state != SyncState::Syncing) return;

        // Mocked progress — see SyncStatus doc comment. Advances a fixed
        // amount per loop tick so the webui has something realistic to
        // poll, without a real sync engine driving it.
        status_.percentComplete = std::min(100, status_.percentComplete + 4);
        status_.etaSeconds = std::max<int64_t>(
            0, (100 - status_.percentComplete) * 3);

        if (status_.percentComplete >= 100) {
            status_.state = SyncState::Idle;
            status_.percentComplete = 0;
            status_.etaSeconds = 0;
            completed = true;
        }
    }
    if (completed) log("info", "Mock sync completed");
}

void SyncManager::log(const std::string &level, const std::string &message) {
    // Self-locking rather than assuming the caller already holds stateMutex_
    // — simpler and safer than tracking which call sites lock first, at the
    // cost of a tiny bit of (harmless, uncontended) re-entrant-looking code
    // at call sites that already hold the lock for an adjacent status_
    // update. Those call sites release the lock before calling log() (see
    // checkHash()/maybeRotatePort()/advanceMockSync()/startMockSync() etc.)
    // to avoid deadlocking on a non-recursive mutex.
    std::lock_guard<std::mutex> lock(stateMutex_);
    logs_.emplace_back(nowIso8601Utc(), level, message);
    while (logs_.size() > kMaxLogEntries) logs_.pop_front();
}

SyncStatus SyncManager::status() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return status_;
}

std::vector<SyncLogEntry> SyncManager::recentLogs(size_t limit) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    size_t n = std::min(limit, logs_.size());
    return std::vector<SyncLogEntry>(logs_.end() - n, logs_.end());
}

// ── mock sync control ─────────────────────────────────────────────────────────

void SyncManager::startMockSync() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status_.state = SyncState::Syncing;
        status_.percentComplete = 0;
        status_.etaSeconds = 100 * 3; // matches advanceMockSync()'s curve
    }
    log("info", "Sync started");
}

void SyncManager::pauseMockSync() {
    bool didPause = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (status_.state == SyncState::Syncing) {
            status_.state = SyncState::Paused;
            didPause = true;
        }
    }
    if (didPause) log("info", "Sync paused by user");
}

void SyncManager::resumeMockSync() {
    bool didResume = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (status_.state == SyncState::Paused) {
            status_.state = SyncState::Syncing;
            didResume = true;
        }
    }
    if (didResume) log("info", "Sync resumed");
}
