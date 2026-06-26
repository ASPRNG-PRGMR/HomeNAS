#pragma once
#include <string>
#include <optional>
#include <cstdint>

// ── event taxonomy ───────────────────────────────────────────────────────────
// Stored as TEXT in SQLite (not an int) so the DB stays human-readable when
// inspected directly (sqlite3 events.db "select * from nas_events"). The
// toString()/fromString() pair below is the single source of truth — add new
// event types here only.

enum class EventType {
    AuthLoginSuccess,
    AuthLoginFailure,
    AuthLogout,
    FileUpload,
    FileDownload,
    FileDelete,
    FileRename,
    FileMove,
    DirCreate,
    DirDelete
};

enum class EventResult {
    Success,
    Failure
};

inline const char *toString(EventType t) {
    switch (t) {
        case EventType::AuthLoginSuccess: return "auth.login.success";
        case EventType::AuthLoginFailure: return "auth.login.failure";
        case EventType::AuthLogout:       return "auth.logout";
        case EventType::FileUpload:       return "file.upload";
        case EventType::FileDownload:     return "file.download";
        case EventType::FileDelete:       return "file.delete";
        case EventType::FileRename:       return "file.rename";
        case EventType::FileMove:         return "file.move";
        case EventType::DirCreate:        return "dir.create";
        case EventType::DirDelete:        return "dir.delete";
    }
    return "unknown";
}

inline const char *toString(EventResult r) {
    return r == EventResult::Success ? "success" : "failure";
}

// ── the event record itself ──────────────────────────────────────────────────
// Mirrors the nas_events table columns 1:1. Fields that don't apply to a given
// event type (e.g. secondary_path for a delete) are left as std::nullopt and
// stored as SQL NULL — cheaper and clearer than empty-string sentinels.
//
// NOTE: this project targets C++17 (see CMakeLists.txt), which does not
// support designated initializers (that's C++20). The fluent setters below
// give call sites the same readable, named-field construction style without
// requiring the newer standard:
//
//   EventRecorder::emit(NasEvent(EventType::FileDelete, EventResult::Success)
//       .withActor(actor)
//       .withSourceIp(req->getPeerAddr().toIp())
//       .withTargetPath(rel));

struct NasEvent {
    EventType type;
    EventResult result;

    std::optional<std::string> actorUser;      // NULL pre-auth (e.g. login failure)
    std::string                sourceIp;       // always known — comes from the request
    std::optional<std::string> claimedUser;    // unverified username on login failure (BF-006)
    std::optional<std::string> targetPath;
    std::optional<std::string> secondaryPath;  // rename/move destination
    std::optional<std::string> failureReason;
    std::optional<int64_t>     bytesTransferred;
    std::optional<int64_t>     durationMs;
    std::optional<std::string> userAgent;
    std::optional<std::string> requestId;

    NasEvent(EventType t, EventResult r) : type(t), result(r) {}

    NasEvent &withActor(std::optional<std::string> v)         { actorUser = std::move(v); return *this; }
    NasEvent &withSourceIp(std::string v)                      { sourceIp = std::move(v); return *this; }
    NasEvent &withClaimedUser(std::optional<std::string> v)   { claimedUser = std::move(v); return *this; }
    NasEvent &withTargetPath(std::optional<std::string> v)    { targetPath = std::move(v); return *this; }
    NasEvent &withSecondaryPath(std::optional<std::string> v) { secondaryPath = std::move(v); return *this; }
    NasEvent &withFailureReason(std::optional<std::string> v) { failureReason = std::move(v); return *this; }
    NasEvent &withBytesTransferred(std::optional<int64_t> v)  { bytesTransferred = v; return *this; }
    NasEvent &withDurationMs(std::optional<int64_t> v)        { durationMs = v; return *this; }
    NasEvent &withUserAgent(std::optional<std::string> v)     { userAgent = std::move(v); return *this; }
    NasEvent &withRequestId(std::optional<std::string> v)     { requestId = std::move(v); return *this; }
};
