#include "AlertWriter.h"
#include <sqlite3.h>
#include <drogon/drogon.h>
#include <filesystem>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>

namespace fs = std::filesystem;

AlertWriter &AlertWriter::instance() {
    static AlertWriter inst;
    return inst;
}

AlertWriter::~AlertWriter() {
    shutdown();
}

void AlertWriter::init(const std::string &alertsDbPath,
                        const std::string &eventsDbPath) {
    fs::create_directories(fs::path(alertsDbPath).parent_path());

    if (sqlite3_open(alertsDbPath.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR << "AlertWriter: failed to open " << alertsDbPath
                  << ": " << sqlite3_errmsg(db_);
        db_ = nullptr;
        return;
    }

    applyAlertSchema();
    applyEventsMigration(eventsDbPath);

    LOG_INFO << "AlertWriter: started, writing to " << alertsDbPath;
}

void AlertWriter::applyAlertSchema() {
    static const char *kSchema = R"SQL(
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous  = NORMAL;

        CREATE TABLE IF NOT EXISTS _migration_guard (
            key TEXT PRIMARY KEY, applied INTEGER
        );
        INSERT OR IGNORE INTO _migration_guard VALUES
            ('nas_events_claimed_user_v1', 0);

        CREATE TABLE IF NOT EXISTS nas_alerts (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_utc   TEXT    NOT NULL,
            rule_id         TEXT    NOT NULL,
            severity        TEXT    NOT NULL,
            title           TEXT    NOT NULL,
            source_ip       TEXT,
            actor_user      TEXT,
            claimed_user    TEXT,
            evidence        TEXT    NOT NULL,
            status          TEXT    NOT NULL DEFAULT 'open',
            resolved_at     TEXT
        );

        CREATE INDEX IF NOT EXISTS idx_alerts_timestamp ON nas_alerts(timestamp_utc);
        CREATE INDEX IF NOT EXISTS idx_alerts_rule      ON nas_alerts(rule_id);
        CREATE INDEX IF NOT EXISTS idx_alerts_severity  ON nas_alerts(severity);
        CREATE INDEX IF NOT EXISTS idx_alerts_status    ON nas_alerts(status);
        CREATE INDEX IF NOT EXISTS idx_alerts_source_ip ON nas_alerts(source_ip);
        CREATE INDEX IF NOT EXISTS idx_alerts_actor     ON nas_alerts(actor_user);
    )SQL";

    char *err = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR << "AlertWriter: schema failed: " << err;
        sqlite3_free(err);
    }
}

void AlertWriter::applyEventsMigration(const std::string &eventsDbPath) {
    // Open events.db briefly just for the migration — EventWriter keeps its
    // own long-lived handle, and we don't want to share that handle across
    // threads. WAL mode makes this safe (concurrent connections are fine).
    sqlite3 *evDb = nullptr;
    if (sqlite3_open(eventsDbPath.c_str(), &evDb) != SQLITE_OK) {
        LOG_WARN << "AlertWriter: couldn't open events.db for migration: "
                  << sqlite3_errmsg(evDb);
        if (evDb) sqlite3_close(evDb);
        return;
    }

    // Check if migration already applied
    sqlite3_stmt *check = nullptr;
    bool alreadyApplied = false;
    const char *checkSql =
        "SELECT applied FROM _migration_guard "
        "WHERE key = 'nas_events_claimed_user_v1'";
    if (sqlite3_prepare_v2(evDb, checkSql, -1, &check, nullptr) == SQLITE_OK) {
        if (sqlite3_step(check) == SQLITE_ROW) {
            alreadyApplied = sqlite3_column_int(check, 0) == 1;
        }
        sqlite3_finalize(check);
    }

    if (!alreadyApplied) {
        // Add claimed_user column to nas_events
        char *err = nullptr;
        int rc = sqlite3_exec(evDb,
            "ALTER TABLE nas_events ADD COLUMN claimed_user TEXT;",
            nullptr, nullptr, &err);
        if (rc == SQLITE_OK) {
            sqlite3_exec(evDb,
                "UPDATE _migration_guard SET applied = 1 "
                "WHERE key = 'nas_events_claimed_user_v1';",
                nullptr, nullptr, nullptr);
            LOG_INFO << "AlertWriter: applied migration — added claimed_user "
                        "to nas_events";
        } else {
            // Most likely "duplicate column name" — means it was added
            // manually or by a previous run without the guard being updated.
            LOG_WARN << "AlertWriter: migration skipped (" << err << ")";
            sqlite3_free(err);
            // Mark as applied anyway so we don't retry every startup
            sqlite3_exec(evDb,
                "INSERT OR REPLACE INTO _migration_guard VALUES "
                "('nas_events_claimed_user_v1', 1);",
                nullptr, nullptr, nullptr);
        }
    }

    sqlite3_close(evDb);
}

void AlertWriter::writeAlert(const NasAlert &alert) {
    if (!db_) return;

    // Timestamp at write time (alert fires after batch analysis,
    // within ~250ms of the triggering events being committed).
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream ts;
    ts << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");

    static const char *kInsert =
        "INSERT INTO nas_alerts "
        "(timestamp_utc, rule_id, severity, title, source_ip, actor_user, "
        " claimed_user, evidence, status) "
        "VALUES (?,?,?,?,?,?,?,?,'open');";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kInsert, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR << "AlertWriter: prepare failed: " << sqlite3_errmsg(db_);
        return;
    }

    auto bindOpt = [&](int idx, const std::optional<std::string> &v) {
        if (v) sqlite3_bind_text(stmt, idx, v->c_str(), -1, SQLITE_TRANSIENT);
        else   sqlite3_bind_null(stmt, idx);
    };

    sqlite3_bind_text(stmt, 1, ts.str().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, alert.ruleId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, toString(alert.severity), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, alert.title.c_str(), -1, SQLITE_TRANSIENT);
    bindOpt(5, alert.sourceIp);
    bindOpt(6, alert.actorUser);
    bindOpt(7, alert.claimedUser);
    sqlite3_bind_text(stmt, 8, alert.evidence.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_ERROR << "AlertWriter: insert failed: " << sqlite3_errmsg(db_);
    }
    sqlite3_finalize(stmt);
}

void AlertWriter::shutdown() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}
