#include "SyncManager.h"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <drogon/drogon.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <functional>
#include <algorithm>
#include <vector>

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

int64_t nowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string epochToIso8601Utc(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// Matches one cron field against one value. Supports "*", "n", "a-b",
// comma-separated lists of those, and a "/step" suffix on any of them.
bool cronFieldMatches(const std::string &field, int value, int lo, int hi) {
    if (field.empty()) return false;

    std::stringstream parts(field);
    std::string part;
    while (std::getline(parts, part, ',')) {
        if (part.empty()) continue;

        int step = 1;
        auto slash = part.find('/');
        if (slash != std::string::npos) {
            try { step = std::stoi(part.substr(slash + 1)); }
            catch (...) { continue; }
            if (step <= 0) continue;
            part = part.substr(0, slash);
        }

        int from, to;
        if (part == "*") {
            from = lo; to = hi;
        } else {
            auto dash = part.find('-');
            try {
                if (dash == std::string::npos) {
                    from = to = std::stoi(part);
                    // A bare "n/step" means "from n to the end of the range,
                    // every step" — not "exactly n".
                    if (slash != std::string::npos) to = hi;
                } else {
                    from = std::stoi(part.substr(0, dash));
                    to   = std::stoi(part.substr(dash + 1));
                }
            } catch (...) { continue; }
        }

        if (from < lo || to > hi || from > to) continue;
        if (value < from || value > to) continue;
        if ((value - from) % step == 0) return true;
    }
    return false;
}

} // namespace

SyncManager &SyncManager::instance() {
    static SyncManager inst;
    return inst;
}

SyncManager::~SyncManager() { shutdown(); }

std::string SyncManager::nowIso8601Utc() {
    return epochToIso8601Utc(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
}

bool SyncManager::cronMatches(const std::string &expr, std::time_t when) {
    if (expr.empty()) return false;

    std::vector<std::string> f;
    std::stringstream ss(expr);
    std::string tok;
    while (ss >> tok) f.push_back(tok);
    if (f.size() != 5) return false;

    std::tm tm{};
    localtime_r(&when, &tm); // schedules are wall-clock local, not UTC

    // Standard cron semantics: when both day-of-month and day-of-week are
    // restricted, the job runs if *either* matches, not both.
    bool domRestricted = (f[2] != "*");
    bool dowRestricted = (f[4] != "*");
    bool domOk = cronFieldMatches(f[2], tm.tm_mday, 1, 31);
    bool dowOk = cronFieldMatches(f[4], tm.tm_wday, 0, 6);
    bool dayOk = (domRestricted && dowRestricted) ? (domOk || dowOk)
                                                  : (domOk && dowOk);

    return cronFieldMatches(f[0], tm.tm_min,  0, 59)
        && cronFieldMatches(f[1], tm.tm_hour, 0, 23)
        && cronFieldMatches(f[3], tm.tm_mon + 1, 1, 12)
        && dayOk;
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

std::string SyncManager::sha256File(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    std::vector<char> buf(65536);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = in.gcount();
        if (n > 0) EVP_DigestUpdate(ctx, buf.data(), (size_t)n);
    }

    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned i = 0; i < len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)out[i];
    return oss.str();
}

bool SyncManager::needsCopy(const std::string &src, const std::string &dst,
                             std::string &reason) {
    std::error_code ec;
    if (!fs::exists(dst, ec)) { reason = "new"; return true; }

    auto srcSize = fs::file_size(src, ec);
    if (ec) { reason = "stat failed"; return true; }
    auto dstSize = fs::file_size(dst, ec);
    if (ec) { reason = "stat failed"; return true; }

    if (srcSize != dstSize) { reason = "size changed"; return true; }

    auto srcTime = fs::last_write_time(src, ec);
    auto dstTime = fs::last_write_time(dst, ec);
    if (ec) { reason = "stat failed"; return true; }
    if (srcTime == dstTime) return false; // fast path: size + mtime agree

    // Same size, different mtime — ambiguous. A touch, a restored file, or a
    // genuine edit that happened to preserve length all look identical here,
    // so fall back to content hashing rather than burning I/O on a copy that
    // may be a no-op.
    std::string a = sha256File(src), b = sha256File(dst);
    if (a.empty() || b.empty()) { reason = "hash failed"; return true; }
    if (a == b) return false;
    reason = "content changed";
    return true;
}

// ── persistence ──────────────────────────────────────────────────────────────

void SyncManager::applySchema() {
    static const char *kSchema = R"SQL(
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous  = NORMAL;

        CREATE TABLE IF NOT EXISTS sync_runs (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            started_utc    TEXT    NOT NULL,
            finished_utc   TEXT    NOT NULL,
            status         TEXT    NOT NULL,
            files_scanned  INTEGER NOT NULL DEFAULT 0,
            files_copied   INTEGER NOT NULL DEFAULT 0,
            files_deleted  INTEGER NOT NULL DEFAULT 0,
            bytes_copied   INTEGER NOT NULL DEFAULT 0,
            duration_ms    INTEGER NOT NULL DEFAULT 0,
            error_count    INTEGER NOT NULL DEFAULT 0,
            last_error     TEXT
        );

        CREATE TABLE IF NOT EXISTS sync_log (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_utc TEXT NOT NULL,
            level         TEXT NOT NULL,
            message       TEXT NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_sync_runs_started ON sync_runs(started_utc);
        CREATE INDEX IF NOT EXISTS idx_sync_log_ts       ON sync_log(timestamp_utc);
    )SQL";

    char *errMsg = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_ERROR << "SyncManager: schema apply failed: "
                  << (errMsg ? errMsg : "(unknown)");
        sqlite3_free(errMsg);
    }
}

void SyncManager::persistRun(const SyncRun &run) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return;

    static const char *kSql =
        "INSERT INTO sync_runs (started_utc, finished_utc, status, "
        "files_scanned, files_copied, files_deleted, bytes_copied, "
        "duration_ms, error_count, last_error) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text (st, 1, run.startedUtc.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, run.finishedUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, run.status.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, run.filesScanned);
    sqlite3_bind_int64(st, 5, run.filesCopied);
    sqlite3_bind_int64(st, 6, run.filesDeleted);
    sqlite3_bind_int64(st, 7, run.bytesCopied);
    sqlite3_bind_int64(st, 8, run.durationMs);
    sqlite3_bind_int64(st, 9, run.errorCount);
    sqlite3_bind_text (st,10, run.lastError.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void SyncManager::loadLastRun() {
    std::lock_guard<std::mutex> dbLock(dbMutex_);
    if (!db_) return;

    static const char *kSql =
        "SELECT finished_utc, status, files_copied, bytes_copied "
        "FROM sync_runs ORDER BY id DESC LIMIT 1";

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) return;
    if (sqlite3_step(st) == SQLITE_ROW) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto txt = [&](int i) {
            const unsigned char *s = sqlite3_column_text(st, i);
            return s ? std::string((const char *)s) : std::string();
        };
        status_.lastRunUtc         = txt(0);
        status_.lastRunStatus      = txt(1);
        status_.lastRunFilesCopied = sqlite3_column_int64(st, 2);
        status_.lastRunBytesCopied = sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
}

void SyncManager::log(const std::string &level, const std::string &message) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO sync_log (timestamp_utc, level, message) VALUES (?,?,?)",
            -1, &st, nullptr) == SQLITE_OK) {
        std::string ts = nowIso8601Utc();
        sqlite3_bind_text(st, 1, ts.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, level.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    // Bounded like the old ring buffer was, just durable. Trimming on every
    // insert keeps the table from growing without a separate vacuum job;
    // the delete is a no-op once the table is at steady state.
    sqlite3_exec(db_,
        ("DELETE FROM sync_log WHERE id <= (SELECT MAX(id) FROM sync_log) - "
         + std::to_string(kMaxLogRows)).c_str(),
        nullptr, nullptr, nullptr);
}

std::vector<SyncLogEntry> SyncManager::recentLogs(size_t limit) const {
    std::vector<SyncLogEntry> out;
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return out;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT timestamp_utc, level, message FROM "
            "(SELECT * FROM sync_log ORDER BY id DESC LIMIT ?) ORDER BY id ASC",
            -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        auto txt = [&](int i) {
            const unsigned char *s = sqlite3_column_text(st, i);
            return s ? std::string((const char *)s) : std::string();
        };
        out.emplace_back(txt(0), txt(1), txt(2));
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<SyncRun> SyncManager::recentRuns(size_t limit) const {
    std::vector<SyncRun> out;
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return out;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id, started_utc, finished_utc, status, files_scanned, "
            "files_copied, files_deleted, bytes_copied, duration_ms, "
            "error_count, last_error FROM sync_runs ORDER BY id DESC LIMIT ?",
            -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        auto txt = [&](int i) {
            const unsigned char *s = sqlite3_column_text(st, i);
            return s ? std::string((const char *)s) : std::string();
        };
        SyncRun r;
        r.id            = sqlite3_column_int64(st, 0);
        r.startedUtc    = txt(1);
        r.finishedUtc   = txt(2);
        r.status        = txt(3);
        r.filesScanned  = sqlite3_column_int64(st, 4);
        r.filesCopied   = sqlite3_column_int64(st, 5);
        r.filesDeleted  = sqlite3_column_int64(st, 6);
        r.bytesCopied   = sqlite3_column_int64(st, 7);
        r.durationMs    = sqlite3_column_int64(st, 8);
        r.errorCount    = sqlite3_column_int64(st, 9);
        r.lastError     = txt(10);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

// ── lifecycle ────────────────────────────────────────────────────────────────

void SyncManager::init(const Config &cfg) {
    cfg_ = cfg;

    std::error_code ec;
    fs::create_directories(fs::path(cfg_.dbPath).parent_path(), ec);

    if (sqlite3_open(cfg_.dbPath.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR << "SyncManager: failed to open " << cfg_.dbPath << ": "
                  << sqlite3_errmsg(db_);
        db_ = nullptr;
    } else {
        applySchema();
        loadLastRun();
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status_.state            = SyncState::Idle;
        status_.lastHash         = computeStorageHash(cfg_.nasRoot);
        status_.lastHashCheckUtc = nowIso8601Utc();
        status_.backupRoot       = cfg_.backupRoot;
    }
    lastHashCheckEpoch_ = nowEpochSeconds();

    // A backup_root inside nas_root would have the engine copying its own
    // output back into itself on every pass, growing without bound. Refuse
    // rather than half-run: the operator has to fix the config.
    if (!cfg_.backupRoot.empty()) {
        auto nasAbs    = fs::weakly_canonical(fs::path(cfg_.nasRoot), ec);
        auto backupAbs = fs::weakly_canonical(fs::path(cfg_.backupRoot), ec);
        auto nasStr    = nasAbs.string();
        auto backupStr = backupAbs.string();
        if (!nasStr.empty() &&
            backupStr.compare(0, nasStr.size(), nasStr) == 0 &&
            (backupStr.size() == nasStr.size() ||
             backupStr[nasStr.size()] == fs::path::preferred_separator)) {
            log("error", "backup_root is inside nas_root (" + backupStr
                          + ") — refusing to run; sync disabled");
            cfg_.backupRoot.clear();
        }
    }

    if (cfg_.backupRoot.empty()) {
        log("warning", "No backup_root configured — sync is status-only");
    } else {
        fs::create_directories(cfg_.backupRoot, ec);
        if (ec) log("error", "Could not create backup_root: " + ec.message());
    }

    log("info", "Sync manager initialized, baseline hash computed");

    running_ = true;
    workerThread_ = std::thread(&SyncManager::runLoop, this);
}

void SyncManager::shutdown() {
    if (!running_) return;
    running_ = false;
    cancelRequested_ = true;
    wakeCv_.notify_all();
    if (workerThread_.joinable()) workerThread_.join();

    std::lock_guard<std::mutex> lock(dbMutex_);
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

void SyncManager::runLoop() {
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wakeCv_.wait_for(lock, std::chrono::seconds(kLoopTickSeconds),
                             [this] { return !running_ || syncRequested_.load(); });
        }
        if (!running_) break;

        maybeRunScheduled();

        if (syncRequested_.exchange(false)) {
            performRun(false);
        } else {
            // Hashing is skipped while a run is in flight — see the class
            // doc comment; the engine's own writes would otherwise look
            // like drift.
            checkHash();
        }
    }
}

// ── control ──────────────────────────────────────────────────────────────────

void SyncManager::requestSync() {
    syncRequested_ = true;
    paused_        = false;
    cancelRequested_ = false;
    wakeCv_.notify_all();
}

void SyncManager::pauseSync()  { paused_ = true;  wakeCv_.notify_all(); }

void SyncManager::resumeSync() { paused_ = false; wakeCv_.notify_all(); }

void SyncManager::cancelSync() {
    cancelRequested_ = true;
    paused_          = false;
    wakeCv_.notify_all();
}

bool SyncManager::waitWhilePaused() {
    if (!paused_) return running_ && !cancelRequested_;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (status_.state == SyncState::Syncing) status_.state = SyncState::Paused;
    }
    log("info", "Sync paused by user");

    while (paused_ && running_ && !cancelRequested_) {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wakeCv_.wait_for(lock, std::chrono::seconds(1));
    }

    if (running_ && !cancelRequested_) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            status_.state = SyncState::Syncing;
        }
        log("info", "Sync resumed");
    }
    return running_ && !cancelRequested_;
}

// ── schedule ─────────────────────────────────────────────────────────────────

void SyncManager::maybeRunScheduled() {
    if (cfg_.scheduleCron.empty() || cfg_.backupRoot.empty()) return;

    std::time_t now = std::time(nullptr);
    int64_t minute  = (int64_t)(now / 60);
    if (minute == lastScheduledRunMinute_) return;   // already handled
    if (!cronMatches(cfg_.scheduleCron, now)) return;

    lastScheduledRunMinute_ = minute;
    log("info", "Scheduled sync triggered (" + cfg_.scheduleCron + ")");
    performRun(true);
}

// ── backup engine ────────────────────────────────────────────────────────────

void SyncManager::performRun(bool scheduled) {
    if (cfg_.backupRoot.empty()) {
        log("error", "Sync requested but no usable backup_root is configured");
        return;
    }

    cancelRequested_ = false;

    SyncRun run;
    run.startedUtc = nowIso8601Utc();
    auto t0 = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status_.state           = SyncState::Syncing;
        status_.percentComplete = 0;
        status_.etaSeconds      = 0;
        status_.filesScanned    = 0;
        status_.filesCopied     = 0;
        status_.bytesCopied     = 0;
        status_.currentFile.clear();
    }
    log("info", std::string(scheduled ? "Scheduled" : "Manual")
                 + " sync started -> " + cfg_.backupRoot);

    std::error_code ec;

    // ── scan ────────────────────────────────────────────────────────────────
    // Collected up front rather than copied during the walk so percent and
    // ETA have a real denominator from the first file.
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(
             cfg_.nasRoot, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) continue;
        if (!running_ || cancelRequested_) break;
        // .nas-meta is HomeNAS's own audit databases, not user content —
        // backing up a live WAL database by file copy produces a corrupt
        // snapshot anyway.
        if (it->path().filename() == ".nas-meta") {
            it.disable_recursion_pending();
            continue;
        }
        if (fs::is_regular_file(it->path(), ec)) files.push_back(it->path());
    }

    run.filesScanned = (int64_t)files.size();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status_.filesScanned = run.filesScanned;
    }

    // ── copy ────────────────────────────────────────────────────────────────
    int64_t processed = 0;
    for (const auto &src : files) {
        if (!waitWhilePaused()) break;

        std::string rel = fs::relative(src, cfg_.nasRoot, ec).string();
        if (ec) { ec.clear(); continue; }
        fs::path dst = fs::path(cfg_.backupRoot) / rel;

        std::string reason;
        if (needsCopy(src.string(), dst.string(), reason)) {
            fs::create_directories(dst.parent_path(), ec);
            ec.clear();

            auto size = fs::file_size(src, ec);
            if (ec) { ec.clear(); size = 0; }

            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                run.errorCount++;
                run.lastError = rel + ": " + ec.message();
                log("error", "Copy failed for " + rel + ": " + ec.message());
                ec.clear();
            } else {
                // Mirror mtime so the next run's fast path recognises this
                // file as unchanged instead of re-hashing it every time.
                auto srcTime = fs::last_write_time(src, ec);
                if (!ec) fs::last_write_time(dst, srcTime, ec);
                ec.clear();

                run.filesCopied++;
                run.bytesCopied += (int64_t)size;
            }
        }

        processed++;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            status_.filesCopied = run.filesCopied;
            status_.bytesCopied = run.bytesCopied;
            status_.currentFile = rel;
            status_.percentComplete = run.filesScanned > 0
                ? (int)((processed * 100) / run.filesScanned) : 100;
            status_.etaSeconds = (processed > 0 && elapsed > 0)
                ? (int64_t)((double)elapsed / processed
                            * (double)(run.filesScanned - processed))
                : 0;
        }
    }

    if (cfg_.deleteFromBackup && running_ && !cancelRequested_) {
        mirrorDeletions(run);
    }

    // ── finish ──────────────────────────────────────────────────────────────
    run.finishedUtc = nowIso8601Utc();
    run.durationMs  = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    run.status = cancelRequested_ ? "cancelled"
               : (run.errorCount > 0 ? "failed" : "completed");

    persistRun(run);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status_.state = (run.status == "failed") ? SyncState::Error
                                                 : SyncState::Idle;
        status_.percentComplete    = 0;
        status_.etaSeconds         = 0;
        status_.currentFile.clear();
        status_.lastRunUtc         = run.finishedUtc;
        status_.lastRunStatus      = run.status;
        status_.lastRunFilesCopied = run.filesCopied;
        status_.lastRunBytesCopied = run.bytesCopied;
    }

    std::ostringstream summary;
    summary << "Sync " << run.status << " — " << run.filesCopied << " copied, "
            << run.bytesCopied << " bytes, " << run.filesScanned << " scanned, "
            << run.durationMs << "ms";
    if (run.filesDeleted) summary << ", " << run.filesDeleted << " removed";
    if (run.errorCount)   summary << ", " << run.errorCount << " errors";
    log(run.errorCount > 0 ? "error" : "info", summary.str());

    notifyRunFinished(run);

    // The copy run didn't touch nas_storage, but it may have taken long
    // enough that the user did. Re-baseline so the next checkHash() compares
    // against the tree as it stands now rather than reporting drift the run
    // merely straddled.
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        status_.lastHash         = computeStorageHash(cfg_.nasRoot);
        status_.lastHashCheckUtc = nowIso8601Utc();
    }
    lastHashCheckEpoch_ = nowEpochSeconds();
}

void SyncManager::mirrorDeletions(SyncRun &run) {
    std::error_code ec;
    std::vector<fs::path> orphans;

    for (auto it = fs::recursive_directory_iterator(
             cfg_.backupRoot, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) continue;
        if (!running_ || cancelRequested_) return;
        if (!fs::is_regular_file(it->path(), ec)) continue;

        std::string rel = fs::relative(it->path(), cfg_.backupRoot, ec).string();
        if (ec) { ec.clear(); continue; }
        if (!fs::exists(fs::path(cfg_.nasRoot) / rel, ec))
            orphans.push_back(it->path());
    }

    // Collected first, deleted after: removing entries while the recursive
    // iterator is walking the same tree is undefined behaviour.
    for (const auto &p : orphans) {
        if (fs::remove(p, ec)) run.filesDeleted++;
        else { run.errorCount++; run.lastError = "delete failed: " + p.string(); }
        ec.clear();
    }
    if (run.filesDeleted)
        log("warning", "Mirrored " + std::to_string(run.filesDeleted)
                        + " deletion(s) into backup (delete_from_backup=true)");
}

void SyncManager::notifyRunFinished(const SyncRun &run) {
    // Phase 2.56 seam: Telegram (or any other notifier) hangs off here, on
    // sync.complete / sync.error. Left as a no-op rather than shipping an
    // unconfigured HTTP client — see devlog.
    (void)run;
}

// ── hashing ──────────────────────────────────────────────────────────────────

void SyncManager::checkHash() {
    int64_t nowEpoch = nowEpochSeconds();
    if (nowEpoch - lastHashCheckEpoch_ < cfg_.hashIntervalSeconds) return;
    lastHashCheckEpoch_ = nowEpoch;

    std::string newHash = computeStorageHash(cfg_.nasRoot);

    bool enteredMismatch = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);

        bool changed = !status_.lastHash.empty() && newHash != status_.lastHash;
        status_.lastHash         = newHash;
        status_.lastHashCheckUtc = nowIso8601Utc();

        if (changed && status_.state != SyncState::HashMismatch &&
            status_.state != SyncState::Syncing) {
            // Storage content changed but the sync engine wasn't running —
            // exactly the "hash changed for no reason we know about" case
            // from the design. This is the one state checkHash() can enter
            // on its own; Error is reserved for sync-engine-reported
            // failures (see performRun()).
            status_.state = SyncState::HashMismatch;
            enteredMismatch = true;
        }
    } // lock released before logging — log() takes dbMutex_, not stateMutex_

    if (enteredMismatch) {
        log("error", "Storage hash changed with no sync in progress — "
                      "possible unverified modification outside HomeNAS");
    }
}

// ── status ───────────────────────────────────────────────────────────────────

SyncStatus SyncManager::status() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    SyncStatus s = status_;

    // Next scheduled run, searched forward a minute at a time. Capped at 48h
    // because every expression this matcher supports repeats within a day —
    // a cron that doesn't fire inside two days (say "0 3 1 1 *") just
    // reports empty rather than scanning a year per status poll.
    if (!cfg_.scheduleCron.empty()) {
        std::time_t t = std::time(nullptr);
        t -= (t % 60);
        for (int i = 1; i <= 60 * 48; ++i) {
            if (cronMatches(cfg_.scheduleCron, t + i * 60)) {
                s.nextRunUtc = epochToIso8601Utc(t + i * 60);
                break;
            }
        }
    }
    return s;
}
