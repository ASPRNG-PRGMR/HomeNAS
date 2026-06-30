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
// A single line in the in-memory error/event log surfaced by the sync icon's
// "click to see logs" panel. Not persisted to SQLite — sync is a separate
// subsystem from the audit trail (events.db/alerts.db) and its log is
// intentionally lightweight, reset on backend restart.
struct SyncLogEntry {
    std::string timestampUtc;
    std::string level;     // "info" | "warning" | "error"
    std::string message;

    SyncLogEntry(std::string ts, std::string lvl, std::string msg)
        : timestampUtc(std::move(ts)), level(std::move(lvl)), message(std::move(msg)) {}
};

// ── sync status snapshot ─────────────────────────────────────────────────────
// What GET /api/sync/status returns. percentComplete/etaSeconds are mocked
// (no real sync engine exists yet — see devlog Phase 3). hash fields are
// real, backed by SyncManager's background hashing thread.
struct SyncStatus {
    SyncState   state          = SyncState::Idle;
    int         percentComplete = 0;      // 0-100, mocked while syncing
    int64_t     etaSeconds      = 0;       // mocked while syncing
    std::string lastHash;                  // last verified hash of nas_storage
    std::string lastHashCheckUtc;
    int         currentPort     = 0;       // current rotating sync-service port
    std::string portRotatedAtUtc;
};
