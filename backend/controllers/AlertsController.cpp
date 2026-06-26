#include "AlertsController.h"
#include <drogon/drogon.h>
#include <sqlite3.h>
#include <json/json.h>
#include <sstream>
#include <vector>

namespace {

std::string alertsDbPath() {
    auto &cfg = drogon::app().getCustomConfig();
    return cfg.get("alerts_db_path",
        "/home/noobiegg/nas/nas_storage/.nas-meta/alerts.db").asString();
}

sqlite3 *openReadOnly(const std::string &path) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr)
            != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    return db;
}

sqlite3 *openReadWrite(const std::string &path) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db,
                         SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    return db;
}

Json::Value rowToJson(sqlite3_stmt *stmt) {
    Json::Value row;
    row["id"]            = sqlite3_column_int(stmt, 0);
    row["timestamp_utc"] = (const char *)sqlite3_column_text(stmt, 1);
    row["rule_id"]       = (const char *)sqlite3_column_text(stmt, 2);
    row["severity"]      = (const char *)sqlite3_column_text(stmt, 3);
    row["title"]         = (const char *)sqlite3_column_text(stmt, 4);
    row["status"]        = (const char *)sqlite3_column_text(stmt, 9);

    auto colOrNull = [&](int idx) -> Json::Value {
        const unsigned char *t = sqlite3_column_text(stmt, idx);
        return t ? Json::Value((const char *)t) : Json::Value(Json::nullValue);
    };
    row["source_ip"]    = colOrNull(5);
    row["actor_user"]   = colOrNull(6);
    row["claimed_user"] = colOrNull(7);
    row["evidence"]     = colOrNull(8);
    row["resolved_at"]  = colOrNull(10);
    return row;
}

constexpr const char *kCols =
    "id, timestamp_utc, rule_id, severity, title, source_ip, actor_user, "
    "claimed_user, evidence, status, resolved_at";

void jsonError(std::function<void(const drogon::HttpResponsePtr &)> &cb,
               drogon::HttpStatusCode code, const std::string &msg) {
    Json::Value e; e["error"] = msg;
    auto r = drogon::HttpResponse::newHttpJsonResponse(e);
    r->setStatusCode(code);
    cb(r);
}

} // namespace

// ── GET /api/alerts ───────────────────────────────────────────────────────────

void AlertsController::list(const drogon::HttpRequestPtr &req,
                             std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    sqlite3 *db = openReadOnly(alertsDbPath());
    if (!db) return jsonError(callback, drogon::k500InternalServerError,
                               "Alerts DB unavailable");

    std::string severity = req->getParameter("severity");
    std::string rule     = req->getParameter("rule");
    std::string status   = req->getParameter("status");
    std::string from     = req->getParameter("from");
    std::string to       = req->getParameter("to");

    int limit = 50, offset = 0;
    if (!req->getParameter("limit").empty())
        limit = std::min(500, std::max(1, std::stoi(req->getParameter("limit"))));
    if (!req->getParameter("offset").empty())
        offset = std::max(0, std::stoi(req->getParameter("offset")));

    std::ostringstream sql;
    sql << "SELECT " << kCols << " FROM nas_alerts WHERE 1=1";
    std::vector<std::string> binds;
    if (!severity.empty()) { sql << " AND severity = ?";       binds.push_back(severity); }
    if (!rule.empty())     { sql << " AND rule_id = ?";        binds.push_back(rule); }
    if (!status.empty())   { sql << " AND status = ?";         binds.push_back(status); }
    if (!from.empty())     { sql << " AND timestamp_utc >= ?"; binds.push_back(from); }
    if (!to.empty())       { sql << " AND timestamp_utc < ?";  binds.push_back(to); }
    sql << " ORDER BY id DESC LIMIT ? OFFSET ?";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return jsonError(callback, drogon::k500InternalServerError, "Query failed");
    }
    int idx = 1;
    for (auto &b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);

    Json::Value items(Json::arrayValue);
    while (sqlite3_step(stmt) == SQLITE_ROW) items.append(rowToJson(stmt));
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    Json::Value out;
    out["items"]  = items;
    out["limit"]  = limit;
    out["offset"] = offset;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}

// ── GET /api/alerts/:id ───────────────────────────────────────────────────────

void AlertsController::get(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                            int id) {
    sqlite3 *db = openReadOnly(alertsDbPath());
    if (!db) return jsonError(callback, drogon::k500InternalServerError,
                               "Alerts DB unavailable");

    std::string sql = std::string("SELECT ") + kCols +
                       " FROM nas_alerts WHERE id = ?";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return jsonError(callback, drogon::k500InternalServerError, "Query failed");
    }
    sqlite3_bind_int(stmt, 1, id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        Json::Value row = rowToJson(stmt);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return callback(drogon::HttpResponse::newHttpJsonResponse(row));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    jsonError(callback, drogon::k404NotFound, "Alert not found");
}

// ── GET /api/alerts/stats/summary ─────────────────────────────────────────────

void AlertsController::summary(const drogon::HttpRequestPtr &req,
                                std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    sqlite3 *db = openReadOnly(alertsDbPath());
    if (!db) return jsonError(callback, drogon::k500InternalServerError,
                               "Alerts DB unavailable");

    // Counts by severity × status (for dashboard header cards)
    const char *sql =
        "SELECT severity, status, COUNT(*) FROM nas_alerts "
        "GROUP BY severity, status";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return jsonError(callback, drogon::k500InternalServerError, "Query failed");
    }

    Json::Value bySeverity(Json::objectValue);
    int64_t totalOpen = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string sev    = (const char *)sqlite3_column_text(stmt, 0);
        std::string status = (const char *)sqlite3_column_text(stmt, 1);
        int64_t count      = sqlite3_column_int64(stmt, 2);
        if (!bySeverity.isMember(sev)) {
            bySeverity[sev]["open"] = 0;
            bySeverity[sev]["investigating"] = 0;
            bySeverity[sev]["dismissed"] = 0;
        }
        bySeverity[sev][status] = (Json::Int64)count;
        if (status == "open") totalOpen += count;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    Json::Value out;
    out["by_severity"] = bySeverity;
    out["total_open"]  = (Json::Int64)totalOpen;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}

// ── PATCH /api/alerts/:id/status ─────────────────────────────────────────────

void AlertsController::update(const drogon::HttpRequestPtr &req,
                               std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                               int id) {
    auto body = req->getJsonObject();
    if (!body) return jsonError(callback, drogon::k400BadRequest, "Bad request");

    std::string newStatus = (*body)["status"].asString();
    if (newStatus != "investigating" && newStatus != "dismissed" && newStatus != "open") {
        return jsonError(callback, drogon::k400BadRequest,
                          "status must be 'open', 'investigating', or 'dismissed'");
    }

    sqlite3 *db = openReadWrite(alertsDbPath());
    if (!db) return jsonError(callback, drogon::k500InternalServerError,
                               "Alerts DB unavailable");

    // Set resolved_at only when transitioning to dismissed
    const char *sql = (newStatus == "dismissed")
        ? "UPDATE nas_alerts SET status = ?, resolved_at = datetime('now') WHERE id = ?"
        : "UPDATE nas_alerts SET status = ?, resolved_at = NULL WHERE id = ?";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return jsonError(callback, drogon::k500InternalServerError, "Update failed");
    }
    sqlite3_bind_text(stmt, 1, newStatus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0;
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (!ok) return jsonError(callback, drogon::k404NotFound, "Alert not found");

    Json::Value out; out["ok"] = true; out["status"] = newStatus;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}
