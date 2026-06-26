-- backend/db/schema_alerts.sql
-- Applied idempotently at startup by AlertWriter::init().
-- Also contains the Phase 2 migration for nas_events (claimed_user column).

PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;

-- ── Phase 2 migration: add claimed_user to nas_events ─────────────────────
-- claimed_user stores the raw username string from a failed login request,
-- BEFORE credential verification. This is deliberately separate from
-- actor_user (which is only set on verified, successful auth) so the
-- attribution integrity of actor_user is preserved in the schema itself.
-- Required for BF-006 (password spraying: same username, multiple IPs).
--
-- ALTER TABLE ADD COLUMN is idempotent-safe in SQLite: if the column
-- already exists this will fail, so we catch that via a workaround below.
-- SQLite doesn't support IF NOT EXISTS on ADD COLUMN until 3.37.0 (2021),
-- so we check the schema table directly.
CREATE TABLE IF NOT EXISTS _migration_guard (key TEXT PRIMARY KEY, applied INTEGER);
INSERT OR IGNORE INTO _migration_guard VALUES ('nas_events_claimed_user_v1', 0);

-- The actual migration is applied at runtime in AlertWriter::init() via
-- C++ code that checks _migration_guard before running ALTER TABLE.
-- See AlertWriter.cpp::applyMigrations() for the guarded execution.

-- ── alerts table ──────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS nas_alerts (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp_utc   TEXT    NOT NULL,
    rule_id         TEXT    NOT NULL,   -- 'BF-001' .. 'BF-006', 'MD-001' .. 'MD-006'
    severity        TEXT    NOT NULL,   -- 'low' | 'medium' | 'high' | 'critical'
    title           TEXT    NOT NULL,
    source_ip       TEXT,               -- NULL for MD rules (actor-primary)
    actor_user      TEXT,               -- NULL for BF-001..004 (IP-primary, no verified actor)
    claimed_user    TEXT,               -- for BF-006: the targeted username across IPs
    evidence        TEXT    NOT NULL,   -- JSON: counts, timestamps, contributing event IDs
    status          TEXT    NOT NULL DEFAULT 'open',  -- 'open'|'investigating'|'dismissed'
    resolved_at     TEXT                -- NULL until status changes
);

CREATE INDEX IF NOT EXISTS idx_alerts_timestamp ON nas_alerts(timestamp_utc);
CREATE INDEX IF NOT EXISTS idx_alerts_rule      ON nas_alerts(rule_id);
CREATE INDEX IF NOT EXISTS idx_alerts_severity  ON nas_alerts(severity);
CREATE INDEX IF NOT EXISTS idx_alerts_status    ON nas_alerts(status);
CREATE INDEX IF NOT EXISTS idx_alerts_source_ip ON nas_alerts(source_ip);
CREATE INDEX IF NOT EXISTS idx_alerts_actor     ON nas_alerts(actor_user);

PRAGMA user_version = 2;
