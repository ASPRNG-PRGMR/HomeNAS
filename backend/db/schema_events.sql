-- backend/db/schema_events.sql
-- Applied idempotently at startup by EventWriter::init(). Safe to re-run.
-- This file is the source of truth for the events schema — keep it in sync
-- with EventTypes.h and EventWriter.cpp if columns ever change.

PRAGMA journal_mode = WAL;     -- allow background writer + concurrent readers
PRAGMA synchronous  = NORMAL;  -- safe with WAL, much faster than FULL

CREATE TABLE IF NOT EXISTS nas_events (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp_utc      TEXT    NOT NULL,        -- ISO 8601 UTC, e.g. 2026-06-20T14:03:11Z
    event_type         TEXT    NOT NULL,        -- see EventTypes.h toString(EventType)
    actor_user         TEXT,                    -- NULL if pre-auth (e.g. failed login)
    source_ip          TEXT    NOT NULL,
    target_path        TEXT,
    secondary_path     TEXT,                    -- rename/move destination
    result             TEXT    NOT NULL,        -- 'success' | 'failure'
    failure_reason     TEXT,
    bytes_transferred  INTEGER,
    duration_ms        INTEGER,
    user_agent         TEXT,
    request_id         TEXT
);

CREATE INDEX IF NOT EXISTS idx_events_timestamp ON nas_events(timestamp_utc);
CREATE INDEX IF NOT EXISTS idx_events_actor     ON nas_events(actor_user);
CREATE INDEX IF NOT EXISTS idx_events_type      ON nas_events(event_type);
CREATE INDEX IF NOT EXISTS idx_events_result    ON nas_events(result);

-- Schema version marker, used by EventWriter::init() to detect future
-- migrations without needing a separate migrations table at this scale.
PRAGMA user_version = 1;
