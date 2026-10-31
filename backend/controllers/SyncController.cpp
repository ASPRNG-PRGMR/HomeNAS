#include "SyncController.h"
#include "../services/SyncManager.h"
#include <drogon/drogon.h>
#include <json/json.h>

namespace {
Json::Value ok() { Json::Value v; v["ok"] = true; return v; }
}

// ── GET /api/sync/status ─────────────────────────────────────────────────────

void SyncController::status(const drogon::HttpRequestPtr &req,
                             std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    SyncStatus s = SyncManager::instance().status();

    Json::Value out;
    out["state"]               = toString(s.state);
    out["percent_complete"]    = s.percentComplete;
    out["eta_seconds"]         = (Json::Int64)s.etaSeconds;
    out["last_hash"]           = s.lastHash;
    out["last_hash_check_utc"] = s.lastHashCheckUtc;

    out["files_scanned"]       = (Json::Int64)s.filesScanned;
    out["files_copied"]        = (Json::Int64)s.filesCopied;
    out["bytes_copied"]        = (Json::Int64)s.bytesCopied;
    out["current_file"]        = s.currentFile;

    out["last_run_utc"]          = s.lastRunUtc;
    out["last_run_status"]       = s.lastRunStatus;
    out["last_run_files_copied"] = (Json::Int64)s.lastRunFilesCopied;
    out["last_run_bytes_copied"] = (Json::Int64)s.lastRunBytesCopied;

    out["backup_root"]         = s.backupRoot;
    out["next_run_utc"]        = s.nextRunUtc;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}

// ── GET /api/sync/logs ────────────────────────────────────────────────────────

void SyncController::logs(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    size_t limit = 100;
    if (!req->getParameter("limit").empty()) {
        int reqLimit = std::stoi(req->getParameter("limit"));
        limit = (size_t)std::min(500, std::max(1, reqLimit));
    }

    auto entries = SyncManager::instance().recentLogs(limit);

    Json::Value items(Json::arrayValue);
    for (auto &e : entries) {
        Json::Value row;
        row["timestamp_utc"] = e.timestampUtc;
        row["level"]         = e.level;
        row["message"]       = e.message;
        items.append(row);
    }

    Json::Value out;
    out["items"] = items;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}

// ── GET /api/sync/runs ────────────────────────────────────────────────────────

void SyncController::runs(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    size_t limit = 50;
    if (!req->getParameter("limit").empty()) {
        int reqLimit = std::stoi(req->getParameter("limit"));
        limit = (size_t)std::min(500, std::max(1, reqLimit));
    }

    Json::Value items(Json::arrayValue);
    for (auto &r : SyncManager::instance().recentRuns(limit)) {
        Json::Value row;
        row["id"]            = (Json::Int64)r.id;
        row["started_utc"]   = r.startedUtc;
        row["finished_utc"]  = r.finishedUtc;
        row["status"]        = r.status;
        row["files_scanned"] = (Json::Int64)r.filesScanned;
        row["files_copied"]  = (Json::Int64)r.filesCopied;
        row["files_deleted"] = (Json::Int64)r.filesDeleted;
        row["bytes_copied"]  = (Json::Int64)r.bytesCopied;
        row["duration_ms"]   = (Json::Int64)r.durationMs;
        row["error_count"]   = (Json::Int64)r.errorCount;
        row["last_error"]    = r.lastError;
        items.append(row);
    }

    Json::Value out;
    out["items"] = items;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}

// ── POST /api/sync/start ──────────────────────────────────────────────────────

void SyncController::start(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    // Returns as soon as the run is queued — the walk/copy happens on
    // SyncManager's worker thread, so this never blocks a Drogon worker for
    // the duration of a backup. Progress is read back via /api/sync/status.
    SyncManager::instance().requestSync();
    callback(drogon::HttpResponse::newHttpJsonResponse(ok()));
}

// ── POST /api/sync/pause ──────────────────────────────────────────────────────

void SyncController::pause(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    SyncManager::instance().pauseSync();
    callback(drogon::HttpResponse::newHttpJsonResponse(ok()));
}

// ── POST /api/sync/resume ─────────────────────────────────────────────────────

void SyncController::resume(const drogon::HttpRequestPtr &req,
                             std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    SyncManager::instance().resumeSync();
    callback(drogon::HttpResponse::newHttpJsonResponse(ok()));
}

// ── POST /api/sync/cancel ─────────────────────────────────────────────────────

void SyncController::cancel(const drogon::HttpRequestPtr &req,
                             std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    SyncManager::instance().cancelSync();
    callback(drogon::HttpResponse::newHttpJsonResponse(ok()));
}
