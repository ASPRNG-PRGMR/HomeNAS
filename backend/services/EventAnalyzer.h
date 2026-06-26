#pragma once
#include "../services/EventTypes.h"
#include <vector>
#include <string>
#include <utility>
#include <unordered_map>

struct sqlite3;

// EventAnalyzer is called by EventWriter::flushBatch() immediately after
// each batch is committed to events.db. It runs all BF/MD rules against
// the just-flushed batch and recent history, then hands any triggered
// alerts to AlertWriter.
//
// Runs on EventWriter's background thread — no separate thread needed.
// All rule queries are cheap COUNT/GROUP BY operations that take
// microseconds at this data volume.
//
// Deduplication: each rule tracks a "last fired" state so one sustained
// attack doesn't generate a flood of identical alerts. See
// shouldFire() and markFired() in the implementation.
class EventAnalyzer {
public:
    static EventAnalyzer &instance();

    // Opens a read-only connection to events.db for rule queries.
    // Call after EventWriter::init() so the DB and schema exist.
    void init(const std::string &eventsDbPath);

    // Called by EventWriter after each successful batch flush.
    // batch: the events just committed, as (NasEvent, timestamp_utc) pairs.
    void analyze(const std::vector<std::pair<NasEvent, std::string>> &batch);

    void shutdown();

    EventAnalyzer(const EventAnalyzer &) = delete;
    EventAnalyzer &operator=(const EventAnalyzer &) = delete;

private:
    EventAnalyzer() = default;
    ~EventAnalyzer();

    // ── brute force rules ─────────────────────────────────────────────────
    void checkBF001_004(const std::string &ip);
    void checkBF005(const std::string &ip, const std::string &successTs);
    void checkBF006();

    // ── mass delete rules ─────────────────────────────────────────────────
    void checkMD001_004(const std::string &actor);
    void checkMD005(const std::string &actor, const std::string &loginTs);
    void checkMD006(const std::string &actor, const std::string &ip,
                     const std::string &path);

    // ── helpers ───────────────────────────────────────────────────────────

    // Count events matching type/ip/actor within windowSeconds of now.
    int64_t countEvents(const char *eventType,
                         const char *ipOrNull,
                         const char *actorOrNull,
                         int windowSeconds);

    // Count distinct IPs that had login failures for a given claimed_user
    // within windowSeconds.
    int64_t countDistinctIpsForUser(const std::string &claimedUser,
                                     int windowSeconds);

    // Count failures from ip before successTs within windowSeconds.
    int64_t countFailuresBeforeSuccess(const std::string &ip,
                                        const std::string &successTs,
                                        int windowSeconds);

    // Count deletions by actor after loginTs within windowSeconds.
    int64_t countDeletesAfterLogin(const std::string &actor,
                                    const std::string &loginTs,
                                    int windowSeconds);

    // Deduplication: returns true if this rule+key hasn't fired recently.
    // key is typically the IP or actor that triggered the rule.
    bool shouldFire(const char *ruleId, const std::string &key,
                     int cooldownSeconds);
    void markFired(const char *ruleId, const std::string &key);

    sqlite3 *db_ = nullptr;

    // In-memory dedup map: "RULE_ID:key" → last fired epoch seconds.
    // Intentionally simple — at 1-3 users the map never grows large.
    std::unordered_map<std::string, int64_t> lastFired_;
};
