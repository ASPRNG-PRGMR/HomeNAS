#include "EventWriter.h"
#include "EventAnalyzer.h"
#include <sqlite3.h>
#include <drogon/drogon.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

EventWriter &EventWriter::instance() {
    static EventWriter inst;
    return inst;
}

EventWriter::~EventWriter() {
    shutdown();
}

void EventWriter::init(const std::string &dbPath) {
    dbPath_ = dbPath;
    fs::create_directories(fs::path(dbPath_).parent_path());

    if (sqlite3_open(dbPath_.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR << "EventWriter: failed to open " << dbPath_ << ": "
                  << sqlite3_errmsg(db_);
        db_ = nullptr;
        return;
    }

    applySchema();

    running_ = true;
    workerThread_ = std::thread(&EventWriter::runLoop, this);
    LOG_INFO << "EventWriter: started, writing to " << dbPath_;
}

void EventWriter::applySchema() {
    // schema_events.sql is the source of truth; this mirrors it so the
    // binary doesn't depend on the .sql file being present at runtime.
    // Keep these two in sync if columns change.
    static const char *kSchema = R"SQL(
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous  = NORMAL;

        CREATE TABLE IF NOT EXISTS nas_events (
            id                 INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_utc      TEXT    NOT NULL,
            event_type         TEXT    NOT NULL,
            actor_user         TEXT,
            source_ip          TEXT    NOT NULL,
            target_path        TEXT,
            secondary_path     TEXT,
            result             TEXT    NOT NULL,
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
    )SQL";

    char *errMsg = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_ERROR << "EventWriter: schema apply failed: " << errMsg;
        sqlite3_free(errMsg);
    }
}

void EventWriter::enqueue(NasEvent event, std::string timestampUtc) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (queue_.size() >= kMaxQueueSize) {
        droppedEvents_++;
        if (droppedEvents_ % 1000 == 1) {
            LOG_WARN << "EventWriter: queue full, dropped "
                     << droppedEvents_.load() << " events so far";
        }
        return;
    }
    queue_.emplace(std::move(event), std::move(timestampUtc));
    queueCv_.notify_one();
}

void EventWriter::runLoop() {
    while (running_) {
        std::vector<std::pair<NasEvent, std::string>> batch;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait_for(lock, std::chrono::milliseconds(kFlushIntervalMs),
                               [this] { return !queue_.empty() || !running_; });

            while (!queue_.empty() && batch.size() < kFlushBatchSize) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop();
            }
        }
        if (!batch.empty()) {
            flushBatch(batch);
        }
    }

    // Final drain on shutdown — don't lose events queued right before exit.
    std::vector<std::pair<NasEvent, std::string>> remaining;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!queue_.empty()) {
            remaining.push_back(std::move(queue_.front()));
            queue_.pop();
        }
    }
    if (!remaining.empty()) {
        flushBatch(remaining);
    }
}

namespace {
void bindOptional(sqlite3_stmt *stmt, int idx, const std::optional<std::string> &v) {
    if (v) sqlite3_bind_text(stmt, idx, v->c_str(), -1, SQLITE_TRANSIENT);
    else   sqlite3_bind_null(stmt, idx);
}
void bindOptional(sqlite3_stmt *stmt, int idx, const std::optional<int64_t> &v) {
    if (v) sqlite3_bind_int64(stmt, idx, *v);
    else   sqlite3_bind_null(stmt, idx);
}
} // namespace

void EventWriter::flushBatch(std::vector<std::pair<NasEvent, std::string>> &batch) {
    if (!db_) return;

    // One transaction per flush — this is the load-bearing optimization.
    // SQLite's per-transaction fsync dominates cost at small batch sizes;
    // batching here is what makes "log every action" affordable.
    sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);

    static const char *kInsert =
        "INSERT INTO nas_events "
        "(timestamp_utc, event_type, actor_user, source_ip, claimed_user, target_path, "
        " secondary_path, result, failure_reason, bytes_transferred, "
        " duration_ms, user_agent, request_id) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kInsert, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR << "EventWriter: prepare failed: " << sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    for (auto &[ev, ts] : batch) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, toString(ev.type), -1, SQLITE_TRANSIENT);
        bindOptional(stmt, 3, ev.actorUser);
        sqlite3_bind_text(stmt, 4, ev.sourceIp.c_str(), -1, SQLITE_TRANSIENT);
        bindOptional(stmt, 5, ev.claimedUser);
        bindOptional(stmt, 6, ev.targetPath);
        bindOptional(stmt, 7, ev.secondaryPath);
        sqlite3_bind_text(stmt, 8, toString(ev.result), -1, SQLITE_TRANSIENT);
        bindOptional(stmt, 9, ev.failureReason);
        bindOptional(stmt, 10, ev.bytesTransferred);
        bindOptional(stmt, 11, ev.durationMs);
        bindOptional(stmt, 12, ev.userAgent);
        bindOptional(stmt, 13, ev.requestId);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            LOG_ERROR << "EventWriter: insert failed: " << sqlite3_errmsg(db_);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

    // Phase 2: run detection rules against the just-committed batch.
    // Runs on this same background thread — cheap COUNT queries, microseconds.
    EventAnalyzer::instance().analyze(batch);
}

void EventWriter::shutdown() {
    if (!running_.exchange(false)) return; // already shut down
    queueCv_.notify_all();
    if (workerThread_.joinable()) workerThread_.join();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}
