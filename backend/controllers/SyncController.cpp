#include "SyncController.h"
#include "../services/SyncManager.h"
#include <drogon/drogon.h>
#include <json/json.h>

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
    out["current_port"]        = s.currentPort;
    out["port_rotated_at_utc"] = s.portRotatedAtUtc;
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

// ── POST /api/sync/start ──────────────────────────────────────────────────────

void SyncController::start(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    SyncManager::instance().startMockSync();
    Json::Value out; out["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}

// ── POST /api/sync/pause ──────────────────────────────────────────────────────

void SyncController::pause(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    SyncManager::instance().pauseMockSync();
    Json::Value out; out["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}

// ── POST /api/sync/resume ─────────────────────────────────────────────────────

void SyncController::resume(const drogon::HttpRequestPtr &req,
                             std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    SyncManager::instance().resumeMockSync();
    Json::Value out; out["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}
