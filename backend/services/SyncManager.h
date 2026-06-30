#pragma once
#include "SyncTypes.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <sys/types.h>

// SyncManager owns the sync subsystem's background thread: periodic
// integrity hashing of nas_storage, and daily rotation of the sync
// service's port.
//
// Threading model mirrors EventWriter — SyncManager owns its own thread
// rather than riding on an existing one (unlike EventAnalyzer, which piggy-
// backs on EventWriter's thread because it only needs to run *after* a
// flush). Sync has no equivalent trigger event; it runs on its own timer.
//
// Hashing: every hashIntervalSeconds_, walks nas_root and computes a single
// aggregate hash (see computeStorageHash() in the .cpp for the exact
// algorithm). If the hash changes between two checks *without* a sync having
// been in progress, that's unexpected drift in the user's storage and the
// state flips to HashMismatch — this is the "hash changed but nobody told
// us why" case called out in the design: only the sync engine should ever
// be the reason storage content changes between checks.
//
// Port rotation: the real sync service does not exist yet (see devlog).
// SyncManager still owns and rotates a port value daily so the API contract
// (GET /api/sync/status returning currentPort) and webui integration can be
// built and tested now, ahead of the real service. When the real sync
// service exists, this is the seam where it will read the rotated port —
// currently nothing consumes it externally.
//
// Portal listener (stopgap): since no real sync service exists to bind the
// rotating port itself, SyncManager also spawns a minimal detached static
// file server (python3 -m http.server) bound to whatever port it just
// rotated to, serving sync-portal/. This is explicitly a placeholder — see
// devlog — and is the one piece of this class that should be deleted
// outright once a real sync engine owns its own port and listener.
class SyncManager {
public:
    static SyncManager &instance();

    // Starts the background thread. Call once at app startup, after
    // EventWriter::init() so nas_root is known to exist on disk.
    // portalDir: directory containing sync-portal's static files
    // (index.html/style.css/app.js) — passed through to the stopgap
    // listener. Empty string disables spawning a listener (status/logs API
    // still works; the portal link just won't resolve to anything, same as
    // before this stopgap existed).
    void init(const std::string &nasRoot,
              int hashIntervalSeconds,
              int portRotationIntervalSeconds,
              const std::string &portalDir = "");

    void shutdown();

    // Snapshot of current state for GET /api/sync/status.
    SyncStatus status() const;

    // Last N log entries (most recent last) for GET /api/sync/logs.
    std::vector<SyncLogEntry> recentLogs(size_t limit) const;

    // ── mock sync control ───────────────────────────────────────────────────
    // No real sync engine exists yet. These let the API/webui simulate state
    // transitions for development — see SyncController for the endpoints
    // that call these. Real implementation will replace these bodies once
    // the sync service exists; the public surface (state machine + status())
    // should not need to change.
    void startMockSync();
    void pauseMockSync();
    void resumeMockSync();

    SyncManager(const SyncManager &) = delete;
    SyncManager &operator=(const SyncManager &) = delete;

private:
    SyncManager() = default;
    ~SyncManager();

    void runLoop();
    void checkHash();
    void maybeRotatePort();
    void advanceMockSync(); // called each loop tick while state == Syncing

    // ── stopgap portal listener ─────────────────────────────────────────────
    // Spawns/kills a detached python3 http.server on the current port,
    // serving portalDir_. See class-level doc comment — this whole section
    // is meant to be deleted once a real sync engine exists.
    void startPortalListener(int port);
    void stopPortalListener();

    void log(const std::string &level, const std::string &message);
    static std::string computeStorageHash(const std::string &root);
    static std::string nowIso8601Utc();

    std::string nasRoot_;
    std::string portalDir_;

    std::thread workerThread_;
    std::atomic<bool> running_{false};

    mutable std::mutex stateMutex_;
    SyncStatus status_;

    // Hashing
    int hashIntervalSeconds_ = 300;
    int64_t lastHashCheckEpoch_ = 0;

    // Port rotation
    int portRotationIntervalSeconds_ = 86400;
    int64_t lastPortRotationEpoch_ = 0;

    // PID of the currently running stopgap portal listener process, or -1
    // if none is running. Guarded by stateMutex_ along with everything else
    // touched during rotation.
    pid_t portalListenerPid_ = -1;

    // In-memory log ring buffer — see SyncLogEntry doc comment for why this
    // isn't persisted to SQLite.
    std::deque<SyncLogEntry> logs_;
    static constexpr size_t kMaxLogEntries = 500;

    static constexpr int kLoopTickSeconds = 5;
};
