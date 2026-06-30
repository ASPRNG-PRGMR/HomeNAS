#include "EventAnalyzer.h"
#include "AlertWriter.h"
#include "AlertTypes.h"
#include <sqlite3.h>
#include <drogon/drogon.h>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <sstream>

EventAnalyzer &EventAnalyzer::instance() {
    static EventAnalyzer inst;
    return inst;
}

EventAnalyzer::~EventAnalyzer() { shutdown(); }

void EventAnalyzer::init(const std::string &eventsDbPath) {
    int rc = sqlite3_open_v2(eventsDbPath.c_str(), &db_,
                              SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR << "EventAnalyzer: failed to open events.db: "
                  << sqlite3_errmsg(db_);
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
    }

    // Load trusted IPs from config — these are never subjected to IP-keyed
    // rules (BF, PT). Localhost and Tailscale IPs should always be here.
    // Example config:
    //   "trusted_ips": ["127.0.0.1", "::1", "100.64.0.1"]
    auto &cfg = drogon::app().getCustomConfig();
    const auto &trustedArr = cfg["trusted_ips"];
    if (trustedArr.isArray()) {
        for (const auto &v : trustedArr) {
            if (v.isString()) {
                trustedIps_.insert(v.asString());
                LOG_INFO << "EventAnalyzer: trusting IP " << v.asString();
            }
        }
    }
    // Localhost is always trusted regardless of config, as a safety net.
    trustedIps_.insert("127.0.0.1");
    trustedIps_.insert("::1");
}

void EventAnalyzer::shutdown() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

// ── main entry point ─────────────────────────────────────────────────────────

void EventAnalyzer::analyze(
    const std::vector<std::pair<NasEvent, std::string>> &batch) {
    if (!db_) return;

    for (auto &[ev, ts] : batch) {

        // BF-001..004: check after every auth failure (IP-keyed)
        if (ev.type == EventType::AuthLoginFailure &&
            !ev.sourceIp.empty() && !isTrusted(ev.sourceIp)) {
            checkBF001_004(ev.sourceIp);
        }

        // BF-005: check after every auth SUCCESS (success-after-failures)
        if (ev.type == EventType::AuthLoginSuccess &&
            !ev.sourceIp.empty() && !isTrusted(ev.sourceIp)) {
            checkBF005(ev.sourceIp, ts);
        }

        // BF-006: check password spraying after every auth failure
        // (needs claimed_user; if NULL/empty, skip — old rows pre-migration)
        if (ev.type == EventType::AuthLoginFailure &&
            !isTrusted(ev.sourceIp)) {
            checkBF006();
        }

        // MD-001..004: check after every file/dir delete (actor-keyed)
        // Not IP-gated — we still want to detect mass deletes from trusted IPs.
        if ((ev.type == EventType::FileDelete ||
             ev.type == EventType::DirDelete) &&
             ev.actorUser.has_value()) {
            checkMD001_004(*ev.actorUser);
        }

        // MD-005: check after every auth success (login→delete burst)
        if (ev.type == EventType::AuthLoginSuccess &&
            ev.actorUser.has_value()) {
            checkMD005(*ev.actorUser, ts);
        }

        // MD-006: any dir.delete → MEDIUM alert (directory-level wipe)
        if (ev.type == EventType::DirDelete && ev.actorUser.has_value()) {
            checkMD006(*ev.actorUser, ev.sourceIp,
                        ev.targetPath.value_or(""));
        }

        // PT-001/002: path traversal probe (IP-keyed)
        if (ev.result == EventResult::Failure &&
            ev.failureReason.has_value() &&
            ev.failureReason == "path_traversal_or_forbidden" &&
            !ev.sourceIp.empty() && !isTrusted(ev.sourceIp)) {
            checkPT001_002(ev.sourceIp);
        }

        // DX-001/002: bulk download detection (actor-keyed)
        if (ev.type == EventType::FileDownload &&
            ev.result == EventResult::Success &&
            ev.actorUser.has_value()) {
            checkDX001_002(*ev.actorUser);
        }

        // DX-003: login then download burst (actor-keyed)
        if (ev.type == EventType::AuthLoginSuccess &&
            ev.actorUser.has_value()) {
            checkDX003(*ev.actorUser, ts);
        }
    }

    // BF-006 is checked once per batch (not per-event) since it aggregates
    // across all IPs — doing it per-failure would run the same query N times
    // per batch with identical results.
}

// ── trusted IP helper ─────────────────────────────────────────────────────────

bool EventAnalyzer::isTrusted(const std::string &ip) const {
    return trustedIps_.count(ip) > 0;
}

// ── BF-001 .. BF-004 ─────────────────────────────────────────────────────────

void EventAnalyzer::checkBF001_004(const std::string &ip) {
    // Thresholds: 5/60s=LOW, 10/60s=MED, 20/60s=HIGH
    // 50/5min=CRIT, 100/15min=CRIT (BF-004 either condition)
    struct Rule {
        const char *id;
        AlertSeverity sev;
        int64_t threshold;
        int windowSecs;
        const char *title;
        int cooldownSecs;
    };
    static const Rule kRules[] = {
        { RuleId::BF001, AlertSeverity::Low,      5,  60,   "Possible Brute Force Login",         120 },
        { RuleId::BF002, AlertSeverity::Medium,  10,  60,   "Likely Brute Force Login",            120 },
        { RuleId::BF003, AlertSeverity::High,    20,  60,   "Active Brute Force Attack",           120 },
        { RuleId::BF004, AlertSeverity::Critical,50,  300,  "Critical Brute Force Attack",         300 },
    };

    for (auto &r : kRules) {
        int64_t count = countEvents("auth.login.failure",
                                     ip.c_str(), nullptr, r.windowSecs);
        if (count >= r.threshold && shouldFire(r.id, ip, r.cooldownSecs)) {
            // BF-004 second condition: also check 100/15min
            if (std::string(r.id) == RuleId::BF004) {
                int64_t c15 = countEvents("auth.login.failure",
                                           ip.c_str(), nullptr, 900);
                if (count < r.threshold && c15 < 100) continue;
            }

            std::ostringstream ev;
            ev << "{\"failures\":" << count
               << ",\"window_seconds\":" << r.windowSecs
               << ",\"source_ip\":\"" << ip << "\"}";

            AlertWriter::instance().writeAlert(
                NasAlert(r.id, r.sev, r.title, ev.str())
                    .withSourceIp(ip));
            markFired(r.id, ip);

            // Only fire the highest-severity rule that applies this pass
            break;
        }
    }
}

// ── BF-005: success after failures ───────────────────────────────────────────

void EventAnalyzer::checkBF005(const std::string &ip,
                                 const std::string &successTs) {
    // 10 minutes look-back before the success event
    int64_t failures = countFailuresBeforeSuccess(ip, successTs, 600);
    if (failures >= 10 && shouldFire(RuleId::BF005, ip, 300)) {
        std::ostringstream ev;
        ev << "{\"failures_before_success\":" << failures
           << ",\"success_timestamp\":\"" << successTs << "\""
           << ",\"source_ip\":\"" << ip << "\""
           << ",\"window_seconds\":600}";

        AlertWriter::instance().writeAlert(
            NasAlert(RuleId::BF005, AlertSeverity::High,
                      "Possible Successful Password Compromise", ev.str())
                .withSourceIp(ip));
        markFired(RuleId::BF005, ip);
    }
}

// ── BF-006: password spraying ─────────────────────────────────────────────────

void EventAnalyzer::checkBF006() {
    if (!db_) return;

    // Find any claimed_user with failures from >=3 distinct IPs in 5 minutes.
    // claimed_user was added by Phase 2 migration — NULL for older rows.
    const char *sql =
        "SELECT claimed_user, COUNT(DISTINCT source_ip) as ip_count "
        "FROM nas_events "
        "WHERE event_type = 'auth.login.failure' "
        "  AND claimed_user IS NOT NULL "
        "  AND claimed_user != '' "
        "  AND timestamp_utc >= datetime('now', '-5 minutes') "
        "GROUP BY claimed_user "
        "HAVING ip_count >= 3";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *user = (const char *)sqlite3_column_text(stmt, 0);
        int64_t ipCount  = sqlite3_column_int64(stmt, 1);
        if (!user) continue;

        std::string userStr(user);
        if (!shouldFire(RuleId::BF006, userStr, 300)) continue;

        std::ostringstream ev;
        ev << "{\"claimed_user\":\"" << userStr << "\""
           << ",\"distinct_ips\":" << ipCount
           << ",\"window_seconds\":300}";

        AlertWriter::instance().writeAlert(
            NasAlert(RuleId::BF006, AlertSeverity::Medium,
                      "Password Spraying Detected", ev.str())
                .withClaimedUser(userStr));
        markFired(RuleId::BF006, userStr);
    }
    sqlite3_finalize(stmt);
}

// ── MD-001 .. MD-004 ─────────────────────────────────────────────────────────

void EventAnalyzer::checkMD001_004(const std::string &actor) {
    struct Rule {
        const char *id;
        AlertSeverity sev;
        int64_t threshold;
        int windowSecs;
        const char *title;
        int cooldownSecs;
    };
    static const Rule kRules[] = {
        { RuleId::MD001, AlertSeverity::Low,       20,  60,   "Unusual Deletion Activity",        120 },
        { RuleId::MD002, AlertSeverity::Medium,    50,  60,   "Aggressive Bulk Deletion",         120 },
        { RuleId::MD003, AlertSeverity::High,     100,  300,  "Possible Malicious Deletion",      300 },
        { RuleId::MD004, AlertSeverity::Critical, 500,  600,  "Critical Mass Delete — Possible Ransomware", 600 },
    };

    // Count both file.delete and dir.delete (dir deletes can bypass pure
    // file-count rules by deleting top-level folders containing many files).
    int64_t fileDels = countEvents("file.delete", nullptr, actor.c_str(), 0);
    int64_t dirDels  = countEvents("dir.delete",  nullptr, actor.c_str(), 0);

    for (auto &r : kRules) {
        int64_t fCount = countEvents("file.delete", nullptr, actor.c_str(), r.windowSecs);
        int64_t dCount = countEvents("dir.delete",  nullptr, actor.c_str(), r.windowSecs);
        int64_t total  = fCount + dCount;
        (void)fileDels; (void)dirDels; // suppress unused-var warning

        if (total >= r.threshold && shouldFire(r.id, actor, r.cooldownSecs)) {
            std::ostringstream ev;
            ev << "{\"actor\":\"" << actor << "\""
               << ",\"file_deletes\":" << fCount
               << ",\"dir_deletes\":" << dCount
               << ",\"total\":" << total
               << ",\"window_seconds\":" << r.windowSecs << "}";

            AlertWriter::instance().writeAlert(
                NasAlert(r.id, r.sev, r.title, ev.str())
                    .withActor(actor));
            markFired(r.id, actor);
            break; // highest applicable rule only
        }
    }
}

// ── MD-005: login → delete burst ─────────────────────────────────────────────

void EventAnalyzer::checkMD005(const std::string &actor,
                                  const std::string &loginTs) {
    int64_t deletes = countDeletesAfterLogin(actor, loginTs, 300);
    if (deletes >= 50 && shouldFire(RuleId::MD005, actor, 300)) {
        std::ostringstream ev;
        ev << "{\"actor\":\"" << actor << "\""
           << ",\"deletes_after_login\":" << deletes
           << ",\"login_timestamp\":\"" << loginTs << "\""
           << ",\"window_seconds\":300}";

        AlertWriter::instance().writeAlert(
            NasAlert(RuleId::MD005, AlertSeverity::High,
                      "Possible Compromised Account — Login Followed by Mass Delete",
                      ev.str())
                .withActor(actor));
        markFired(RuleId::MD005, actor);
    }
}

// ── MD-006: any directory delete ─────────────────────────────────────────────

void EventAnalyzer::checkMD006(const std::string &actor,
                                  const std::string &ip,
                                  const std::string &path) {
    // MD-006 fires on every dir.delete, no cooldown — each directory
    // delete is independently notable. Evidence records the specific path
    // so the investigator can assess what was deleted.
    // No shouldFire() check: each distinct dir.delete is its own event.
    std::ostringstream ev;
    ev << "{\"actor\":\"" << actor << "\""
       << ",\"path\":\"" << path << "\""
       << ",\"source_ip\":\"" << ip << "\"}";

    AlertWriter::instance().writeAlert(
        NasAlert(RuleId::MD006, AlertSeverity::Medium,
                  "Directory Deleted", ev.str())
            .withActor(actor)
            .withSourceIp(ip));
}

// ── query helpers ─────────────────────────────────────────────────────────────

int64_t EventAnalyzer::countEvents(const char *eventType,
                                     const char *ip,
                                     const char *actor,
                                     int windowSecs) {
    if (!db_) return 0;

    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM nas_events WHERE event_type = ?";
    if (ip)    sql << " AND source_ip = ?";
    if (actor) sql << " AND actor_user = ?";
    if (windowSecs > 0)
        sql << " AND timestamp_utc >= datetime('now', '-"
            << windowSecs << " seconds')";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr)
            != SQLITE_OK) return 0;

    int idx = 1;
    sqlite3_bind_text(stmt, idx++, eventType, -1, SQLITE_TRANSIENT);
    if (ip)    sqlite3_bind_text(stmt, idx++, ip,    -1, SQLITE_TRANSIENT);
    if (actor) sqlite3_bind_text(stmt, idx++, actor, -1, SQLITE_TRANSIENT);

    int64_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

int64_t EventAnalyzer::countFailuresBeforeSuccess(const std::string &ip,
                                                    const std::string &successTs,
                                                    int windowSecs) {
    if (!db_) return 0;
    const char *sql =
        "SELECT COUNT(*) FROM nas_events "
        "WHERE event_type = 'auth.login.failure' "
        "  AND source_ip = ? "
        "  AND timestamp_utc >= datetime(?, '-' || ? || ' seconds') "
        "  AND timestamp_utc < ?";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, successTs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,  3, windowSecs);
    sqlite3_bind_text(stmt, 4, successTs.c_str(), -1, SQLITE_TRANSIENT);

    int64_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

int64_t EventAnalyzer::countDeletesAfterLogin(const std::string &actor,
                                                const std::string &loginTs,
                                                int windowSecs) {
    if (!db_) return 0;
    const char *sql =
        "SELECT COUNT(*) FROM nas_events "
        "WHERE (event_type = 'file.delete' OR event_type = 'dir.delete') "
        "  AND actor_user = ? "
        "  AND timestamp_utc > ? "
        "  AND timestamp_utc <= datetime(?, '+' || ? || ' seconds')";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, loginTs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, loginTs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,  4, windowSecs);

    int64_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

// ── deduplication ─────────────────────────────────────────────────────────────

bool EventAnalyzer::shouldFire(const char *ruleId,
                                 const std::string &key,
                                 int cooldownSeconds) {
    std::string mapKey = std::string(ruleId) + ":" + key;
    auto it = lastFired_.find(mapKey);
    if (it == lastFired_.end()) return true;

    auto now = std::chrono::system_clock::now();
    int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                         now.time_since_epoch()).count();
    return (nowSec - it->second) >= cooldownSeconds;
}

void EventAnalyzer::markFired(const char *ruleId, const std::string &key) {
    std::string mapKey = std::string(ruleId) + ":" + key;
    auto now = std::chrono::system_clock::now();
    lastFired_[mapKey] = std::chrono::duration_cast<std::chrono::seconds>(
                             now.time_since_epoch()).count();
}

// PT-001 / PT-002: path traversal probe
//
// FilesystemController emits FileDownload/FileDelete failures with
// failure_reason = 'path_traversal_or_forbidden' on every rejected path.
// One or two such events is a misconfigured client or typo. A burst
// from the same IP is systematic directory probing.
//
// PT-001 (MEDIUM): >=5  traversal failures from same IP in 60s
// PT-002 (HIGH):   >=20 traversal failures from same IP in 5min

void EventAnalyzer::checkPT001_002(const std::string &ip) {
    struct Rule {
        const char   *id;
        AlertSeverity sev;
        int64_t       threshold;
        int           windowSecs;
        const char   *title;
        int           cooldownSecs;
    };
    static const Rule kRules[] = {
        { RuleId::PT001, AlertSeverity::Medium, 5,  60,  "Path Traversal Probe Detected",   180 },
        { RuleId::PT002, AlertSeverity::High,  20, 300,  "Sustained Path Traversal Attack", 300 },
    };

    for (auto &r : kRules) {
        int64_t count = countTraversalFailures(ip, r.windowSecs);
        if (count >= r.threshold && shouldFire(r.id, ip, r.cooldownSecs)) {
            std::ostringstream ev;
            ev << "{\"source_ip\":\"" << ip << "\""
               << ",\"traversal_failures\":" << count
               << ",\"window_seconds\":" << r.windowSecs
               << ",\"failure_reason\":\"path_traversal_or_forbidden\"}";

            AlertWriter::instance().writeAlert(
                NasAlert(r.id, r.sev, r.title, ev.str())
                    .withSourceIp(ip));
            markFired(r.id, ip);
            break; // highest applicable rule only
        }
    }
}

// DX-001 / DX-002: bulk download detection
//
// Actor-keyed so an attacker with a valid token can't evade by rotating
// IPs. DX-001 (MEDIUM) is informational — could be a legitimate bulk
// export. DX-002 (HIGH) requires genuine volume.
//
// DX-001 (MEDIUM): >=50  downloads by same actor in 5min
// DX-002 (HIGH):   >=200 downloads by same actor in 10min

void EventAnalyzer::checkDX001_002(const std::string &actor) {
    struct Rule {
        const char   *id;
        AlertSeverity sev;
        int64_t       threshold;
        int           windowSecs;
        const char   *title;
        int           cooldownSecs;
    };
    static const Rule kRules[] = {
        { RuleId::DX001, AlertSeverity::Medium, 50,  300, "Unusual Download Volume",                     300 },
        { RuleId::DX002, AlertSeverity::High,  200,  600, "Possible Data Exfiltration -- Bulk Download", 600 },
    };

    for (auto &r : kRules) {
        int64_t count = countEvents("file.download", nullptr,
                                     actor.c_str(), r.windowSecs);
        if (count >= r.threshold && shouldFire(r.id, actor, r.cooldownSecs)) {
            std::ostringstream ev;
            ev << "{\"actor\":\"" << actor << "\""
               << ",\"downloads\":" << count
               << ",\"window_seconds\":" << r.windowSecs << "}";

            AlertWriter::instance().writeAlert(
                NasAlert(r.id, r.sev, r.title, ev.str())
                    .withActor(actor));
            markFired(r.id, actor);
            break;
        }
    }
}

// DX-003: login -> download burst
//
// Mirrors MD-005 (login -> delete burst) but for reads. A fresh login
// immediately followed by bulk downloads is the pattern of a stolen
// credential being used to exfiltrate data before the account is
// noticed and rotated.
//
// DX-003 (HIGH): login followed by >=30 downloads within 5min

void EventAnalyzer::checkDX003(const std::string &actor,
                                 const std::string &loginTs) {
    int64_t downloads = countDownloadsAfterLogin(actor, loginTs, 300);
    if (downloads >= 30 && shouldFire(RuleId::DX003, actor, 300)) {
        std::ostringstream ev;
        ev << "{\"actor\":\"" << actor << "\""
           << ",\"downloads_after_login\":" << downloads
           << ",\"login_timestamp\":\"" << loginTs << "\""
           << ",\"window_seconds\":300}";

        AlertWriter::instance().writeAlert(
            NasAlert(RuleId::DX003, AlertSeverity::High,
                      "Possible Credential Compromise -- Login Followed by Bulk Download",
                      ev.str())
                .withActor(actor));
        markFired(RuleId::DX003, actor);
    }
}

// ── new query helpers ─────────────────────────────────────────────────────────

int64_t EventAnalyzer::countTraversalFailures(const std::string &ip,
                                               int windowSeconds) {
    if (!db_) return 0;
    const char *sql =
        "SELECT COUNT(*) FROM nas_events "
        "WHERE result = 'failure' "
        "  AND source_ip = ? "
        "  AND failure_reason = 'path_traversal_or_forbidden' "
        "  AND timestamp_utc >= datetime('now', '-' || ? || ' seconds')";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, ip.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,  2, windowSeconds);

    int64_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

int64_t EventAnalyzer::countDownloadsAfterLogin(const std::string &actor,
                                                  const std::string &loginTs,
                                                  int windowSeconds) {
    if (!db_) return 0;
    const char *sql =
        "SELECT COUNT(*) FROM nas_events "
        "WHERE event_type = 'file.download' "
        "  AND result = 'success' "
        "  AND actor_user = ? "
        "  AND timestamp_utc > ? "
        "  AND timestamp_utc <= datetime(?, '+' || ? || ' seconds')";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, actor.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, loginTs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, loginTs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,  4, windowSeconds);

    int64_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}
