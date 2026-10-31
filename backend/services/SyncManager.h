#pragma once
#include "SyncTypes.h"
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <ctime>

struct sqlite3; // fwd-decl, avoids forcing <sqlite3.h> on every includer

// SyncManager owns the sync subsystem: periodic integrity hashing of
// nas_storage, and the backup engine itself.
//
// "Sync" here means **one-way backup of nas_storage to backup_root** (see
// devlog Phase 3.2 for the options that were ruled out). Deletions in
// nas_storage are deliberately NOT mirrored by default: the backup is a
// safety net, not a mirror, so a ransomware wipe of nas_storage leaves the
// backup intact through the next run. delete_from_backup opts into mirroring.
//
// Threading model mirrors EventWriter — SyncManager owns its own thread
// rather than riding on an existing one (unlike EventAnalyzer, which piggy-
// backs on EventWriter's thread because it only needs to run *after* a
// flush). Sync has no equivalent trigger event; it runs on its own timer.
//
// The worker thread does everything: hashing, schedule evaluation, and the
// copy run. A run therefore blocks hashing for its duration, which is
// intentional — hashing mid-run would read a tree the engine is actively
// writing to and report drift that the engine itself caused.
//
// Hashing: every hashIntervalSeconds_, walks nas_root and computes a single
// aggregate hash (see computeStorageHash() in the .cpp for the exact
// algorithm). If the hash changes between two checks *without* a sync having
// been in progress, that's unexpected drift in the user's storage and the
// state flips to HashMismatch — this is the "hash changed but nobody told
// us why" case called out in the design: only the sync engine should ever
// be the reason storage content changes between checks.
//
// SQLite access is guarded by dbMutex_ rather than funnelled through a
// producer queue the way EventWriter does it. EventWriter needs the queue
// because it's written from request hot paths; sync writes a handful of rows
// per run, so a plain mutex is the simpler correct thing here.
class SyncManager {
public:
    static SyncManager &instance();

    struct Config {
        std::string nasRoot;
        std::string backupRoot;
        std::string dbPath;
        int         hashIntervalSeconds = 300;
        std::string scheduleCron;          // 5-field cron; empty = manual only
        bool        deleteFromBackup = false;
    };

    // Starts the background thread. Call once at app startup, after
    // EventWriter::init() so nas_root is known to exist on disk.
    void init(const Config &cfg);

    void shutdown();

    // Snapshot of current state for GET /api/sync/status.
    SyncStatus status() const;

    // Last N log entries (most recent last) for GET /api/sync/logs.
    std::vector<SyncLogEntry> recentLogs(size_t limit) const;

    // Last N finished runs (most recent first) for GET /api/sync/runs.
    std::vector<SyncRun> recentRuns(size_t limit) const;

    // Minimal 5-field cron matcher: supports *, n, a-b, a,b,c and */step.
    // Public only because it's a pure function worth testing directly —
    // nothing outside SyncManager calls it in production.
    static bool cronMatches(const std::string &expr, std::time_t when);

    // ── sync control ────────────────────────────────────────────────────────
    // Called from Drogon worker threads via SyncController. These only set
    // flags and wake the worker; the run itself always happens on the
    // worker thread.
    void requestSync();
    void pauseSync();
    void resumeSync();
    void cancelSync();

    SyncManager(const SyncManager &) = delete;
    SyncManager &operator=(const SyncManager &) = delete;

private:
    SyncManager() = default;
    ~SyncManager();

    void runLoop();
    void checkHash();
    void maybeRunScheduled();

    // ── backup engine ───────────────────────────────────────────────────────
    void performRun(bool scheduled);
    // Returns false if the run should stop early (cancelled or shutting
    // down). Blocks while paused.
    bool waitWhilePaused();
    // True if src should be copied over dst. Fast path is size + mtime;
    // falls back to SHA-256 only when sizes match but mtimes differ, which
    // is the ambiguous case where mtime alone would copy needlessly.
    static bool needsCopy(const std::string &src, const std::string &dst,
                          std::string &reason);
    static std::string sha256File(const std::string &path);
    void mirrorDeletions(SyncRun &run);

    // ── persistence ─────────────────────────────────────────────────────────
    void applySchema();
    void persistRun(const SyncRun &run);
    void loadLastRun();

    // Notification seam. Called once per finished run, inside the worker
    // thread. Phase 2.56 wires Telegram here — see devlog; deliberately a
    // no-op rather than an unconfigured HTTP client shipping dormant.
    void notifyRunFinished(const SyncRun &run);

    void log(const std::string &level, const std::string &message);
    static std::string computeStorageHash(const std::string &root);
    static std::string nowIso8601Utc();

    Config cfg_;

    std::thread workerThread_;
    std::atomic<bool> running_{false};

    // Control flags, set from API threads, consumed by the worker.
    std::atomic<bool> syncRequested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> cancelRequested_{false};

    // Wakes the worker early when a control flag changes, so a manual start
    // doesn't wait out the remainder of the tick.
    mutable std::mutex wakeMutex_;
    std::condition_variable wakeCv_;

    mutable std::mutex stateMutex_;
    SyncStatus status_;

    mutable std::mutex dbMutex_;
    sqlite3 *db_ = nullptr;

    int64_t lastHashCheckEpoch_ = 0;
    // Minute-resolution guard so a cron expression matching the current
    // minute fires once, not once per tick within that minute.
    int64_t lastScheduledRunMinute_ = -1;

    static constexpr int kLoopTickSeconds = 5;
    static constexpr size_t kMaxLogRows = 2000;
};
