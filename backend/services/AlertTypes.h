#pragma once
#include <string>
#include <optional>
#include <cstdint>

// ── severity ─────────────────────────────────────────────────────────────────
enum class AlertSeverity {
    Low,
    Medium,
    High,
    Critical
};

inline const char *toString(AlertSeverity s) {
    switch (s) {
        case AlertSeverity::Low:      return "low";
        case AlertSeverity::Medium:   return "medium";
        case AlertSeverity::High:     return "high";
        case AlertSeverity::Critical: return "critical";
    }
    return "unknown";
}

// ── rule IDs ─────────────────────────────────────────────────────────────────
// String constants rather than an enum, since rule IDs are stored as TEXT
// and displayed directly. Using constexpr avoids magic strings scattered
// across EventAnalyzer.cpp.
namespace RuleId {
    // Brute force
    constexpr const char *BF001 = "BF-001";  // >=5  failures/60s  from same IP — LOW
    constexpr const char *BF002 = "BF-002";  // >=10 failures/60s  from same IP — MEDIUM
    constexpr const char *BF003 = "BF-003";  // >=20 failures/60s  from same IP — HIGH
    constexpr const char *BF004 = "BF-004";  // >=50/5min OR >=100/15min         — CRITICAL
    constexpr const char *BF005 = "BF-005";  // >=10 failures then success        — HIGH
    constexpr const char *BF006 = "BF-006";  // same user, failures from >=3 IPs  — MEDIUM

    // Mass delete
    constexpr const char *MD001 = "MD-001";  // >=20  deletes/60s  same actor — LOW
    constexpr const char *MD002 = "MD-002";  // >=50  deletes/60s  same actor — MEDIUM
    constexpr const char *MD003 = "MD-003";  // >=100 deletes/5min same actor — HIGH
    constexpr const char *MD004 = "MD-004";  // >=500 deletes/10min same actor — CRITICAL
    constexpr const char *MD005 = "MD-005";  // login then >=50 deletes/5min  — HIGH
    constexpr const char *MD006 = "MD-006";  // any dir.delete                — MEDIUM
}

// ── alert record ─────────────────────────────────────────────────────────────
// Mirrors nas_alerts table columns 1:1. evidence is a pre-serialized JSON
// string — the analyzer builds it, AlertWriter stores it verbatim.
struct NasAlert {
    std::string    ruleId;
    AlertSeverity  severity;
    std::string    title;
    std::string    evidence;       // JSON string

    std::optional<std::string> sourceIp;
    std::optional<std::string> actorUser;
    std::optional<std::string> claimedUser;  // BF-006 only

    // Fluent builder — same C++17-compatible pattern as NasEvent
    NasAlert(std::string rid, AlertSeverity sev, std::string t, std::string ev)
        : ruleId(std::move(rid)), severity(sev),
          title(std::move(t)), evidence(std::move(ev)) {}

    NasAlert &withSourceIp(std::optional<std::string> v)   { sourceIp = std::move(v); return *this; }
    NasAlert &withActor(std::optional<std::string> v)      { actorUser = std::move(v); return *this; }
    NasAlert &withClaimedUser(std::optional<std::string> v){ claimedUser = std::move(v); return *this; }
};
