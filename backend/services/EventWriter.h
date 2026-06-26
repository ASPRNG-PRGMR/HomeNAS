#pragma once
#include "EventTypes.h"
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

struct sqlite3; // fwd-decl, avoids forcing <sqlite3.h> on every includer

// EventWriter owns the *only* SQLite connection used for events.db and the
// *only* thread allowed to touch it. This sidesteps SQLite's multi-thread
// handle-sharing footguns entirely rather than relying on SQLITE_OPEN_FULLMUTEX.
//
// Producers (Drogon worker threads, via EventRecorder::emit) push onto a
// thread-safe queue and return immediately. This thread drains the queue on
// a timer/threshold and writes in a single transaction per flush, which is
// what makes this safe to call from hot paths like upload/download without
// adding per-request fsync latency.
class EventWriter {
public:
    static EventWriter &instance();

    // Opens events.db at dbPath, applies schema_events.sql if needed, starts
    // the background thread. Call once at app startup (see main.cpp wiring).
    void init(const std::string &dbPath);

    // Producer-side. Cheap: lock, push, notify, return.
    void enqueue(NasEvent event, std::string timestampUtc);

    // Graceful shutdown: flushes any remaining queued events, joins thread.
    // Call from main.cpp before drogon::app().run() returns / on signal.
    void shutdown();

    EventWriter(const EventWriter &) = delete;
    EventWriter &operator=(const EventWriter &) = delete;

private:
    EventWriter() = default;
    ~EventWriter();

    void runLoop();
    void flushBatch(std::vector<std::pair<NasEvent, std::string>> &batch);
    void applySchema();

    sqlite3 *db_ = nullptr;
    std::string dbPath_;

    std::queue<std::pair<NasEvent, std::string>> queue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;

    std::thread workerThread_;
    std::atomic<bool> running_{false};

    // Defensive bound (see Performance Considerations): if the writer ever
    // falls behind at this scale something else is wrong, but we drop with a
    // counter rather than grow unbounded on a laptop with limited RAM.
    static constexpr size_t kMaxQueueSize = 50000;
    std::atomic<uint64_t> droppedEvents_{0};

    static constexpr int kFlushIntervalMs = 250;
    static constexpr size_t kFlushBatchSize = 200;
};
