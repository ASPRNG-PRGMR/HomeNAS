#pragma once
#include <string>
#include <cstdint>

// ── sync state ───────────────────────────────────────────────────────────────
enum class SyncState {
    Idle,
    Syncing,
    Paused,
    Error,
    HashMismatch
};

inline const char *toString(SyncState s) {
    switch (s) {
        case SyncState::Idle:         return "idle";
        case SyncState::Syncing:      return "syncing";
        case SyncState::Paused:       return "paused";
        case SyncState::Error:        return "error";
        case SyncState::HashMismatch: return "hash_mismatch";
    }
    return "unknown";
}

// ── sync log entry ──────────────────────────────────────────────────────────
// A single line in the error/event log surfaced by the sync icon's "click to
// see logs" panel. Persisted to sync.db (sync_log) rather than held in a
// std::deque: the in-memory ring buffer lost every entry on backend restart,
// which is precisely when you most want to read back why sync failed.
struct SyncLogEntry {
    std::string timestampUtc;
    std::string level;     // "info" | "warning" | "error"
    std::string message;

    SyncLogEntry(std::string ts, std::string lvl, std::string msg)
        : timestampUtc(std::move(ts)), level(std::move(lvl)), message(std::move(msg)) {}
};

// ── sync run record ──────────────────────────────────────────────────────────
// Outcome of one backup run, persisted to sync.db (sync_runs). One row per
// run, written when the run reaches a terminal state.
struct SyncRun {
    int64_t     id             = 0;
    std::string startedUtc;
    std::string finishedUtc;
    std::string status;            // "completed" | "failed" | "cancelled"
    int64_t     filesScanned   = 0;
    int64_t     filesCopied    = 0;
    int64_t     filesDeleted   = 0;  // only non-zero when delete_from_backup
    int64_t     bytesCopied    = 0;
    int64_t     durationMs     = 0;
    int64_t     errorCount     = 0;
    std::string lastError;
};

// ── sync status snapshot ─────────────────────────────────────────────────────
// What GET /api/sync/status returns. percentComplete/etaSeconds are driven by
// the real copy engine (files processed against the run's scanned total), not
// mocked. hash fields are backed by SyncManager's background hashing thread.
struct SyncStatus {
    SyncState   state           = SyncState::Idle;
    int         percentComplete = 0;      // 0-100 across the active run
    int64_t     etaSeconds      = 0;      // extrapolated from observed throughput
    std::string lastHash;                 // last verified hash of nas_storage
    std::string lastHashCheckUtc;

    // Live counters for the run in progress; reset at the start of each run.
    int64_t     filesScanned    = 0;
    int64_t     filesCopied     = 0;
    int64_t     bytesCopied     = 0;
    std::string currentFile;              // relative path being copied

    // Summary of the most recent finished run, for the idle-state tooltip.
    std::string lastRunUtc;
    std::string lastRunStatus;
    int64_t     lastRunFilesCopied = 0;
    int64_t     lastRunBytesCopied = 0;

    std::string backupRoot;
    std::string nextRunUtc;               // empty when no schedule configured
};
