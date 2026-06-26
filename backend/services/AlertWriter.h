#pragma once
#include "AlertTypes.h"
#include <string>

struct sqlite3;

// AlertWriter owns the only SQLite connection to alerts.db (same single-owner
// pattern as EventWriter for events.db). Unlike EventWriter it doesn't need a
// background thread or queue — alerts are rare (a handful per incident, not
// thousands per second like events), so synchronous writes on EventWriter's
// already-background thread are fine.
//
// Also handles the Phase 2 schema migration: adding claimed_user to nas_events
// in the events.db file, guarded by _migration_guard to be idempotent.
class AlertWriter {
public:
    static AlertWriter &instance();

    // Opens alerts.db at alertsDbPath. Also opens events.db at eventsDbPath
    // briefly to apply the claimed_user migration, then closes it — the
    // long-lived events.db connection stays owned by EventWriter as before.
    void init(const std::string &alertsDbPath, const std::string &eventsDbPath);

    // Write one alert. Called from EventAnalyzer on EventWriter's thread —
    // no locking needed since AlertWriter is only ever touched from there.
    void writeAlert(const NasAlert &alert);

    void shutdown();

    AlertWriter(const AlertWriter &) = delete;
    AlertWriter &operator=(const AlertWriter &) = delete;

private:
    AlertWriter() = default;
    ~AlertWriter();

    void applyAlertSchema();
    void applyEventsMigration(const std::string &eventsDbPath);

    sqlite3 *db_ = nullptr;
};
