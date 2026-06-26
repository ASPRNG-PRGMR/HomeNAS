#include "EventsController.h"
#include "../services/EventWriter.h"
#include <drogon/drogon.h>
#include <sqlite3.h>
#include <json/json.h>
#include <sstream>
#include <vector>

namespace {

// Each request opens its own read-only SQLite connection. This is safe and
// standard practice under WAL mode (the mode EventWriter enables at startup):
// one writer + many readers can operate concurrently without blocking each
// other. Routing reads through EventWriter's single thread instead would
// needlessly serialize them behind the write queue.
//
// dbPath must match what EventWriter::init() was given in main.cpp.
sqlite3 *openReadOnly(const std::string &dbPath) {
    sqlite3 *db = nullptr;
    int rc = sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    return db;
}

std::string dbPathFromConfig() {
    auto &cfg = drogon::app().getCustomConfig();
    // Mirrors the default used when wiring EventWriter::init() in main.cpp —
    // keep these in sync if the config key or default ever changes.
    return cfg.get("events_db_path", "/home/noobiegg/nas/nas_storage/.nas-meta/events.db").asString();
}

Json::Value rowToJson(sqlite3_stmt *stmt) {
    Json::Value row;
    row["id"]                = sqlite3_column_int(stmt, 0);
    row["timestamp_utc"]     = (const char *)sqlite3_column_text(stmt, 1);
    row["event_type"]        = (const char *)sqlite3_column_text(stmt, 2);

    auto colOrNull = [&](int idx) -> Json::Value {
        const unsigned char *txt = sqlite3_column_text(stmt, idx);
        return txt ? Json::Value((const char *)txt) : Json::Value(Json::nullValue);
    };

    row["actor_user"]        = colOrNull(3);
    row["source_ip"]         = (const char *)sqlite3_column_text(stmt, 4);
    row["target_path"]       = colOrNull(5);
    row["secondary_path"]    = colOrNull(6);
    row["result"]            = (const char *)sqlite3_column_text(stmt, 7);
    row["failure_reason"]    = colOrNull(8);

    if (sqlite3_column_type(stmt, 9) != SQLITE_NULL)
        row["bytes_transferred"] = (Json::Int64)sqlite3_column_int64(stmt, 9);
    else
        row["bytes_transferred"] = Json::Value(Json::nullValue);

    if (sqlite3_column_type(stmt, 10) != SQLITE_NULL)
        row["duration_ms"] = (Json::Int64)sqlite3_column_int64(stmt, 10);
    else
        row["duration_ms"] = Json::Value(Json::nullValue);

    row["user_agent"]        = colOrNull(11);
    row["request_id"]        = colOrNull(12);
    return row;
}

constexpr const char *kSelectColumns =
    "id, timestamp_utc, event_type, actor_user, source_ip, target_path, "
    "secondary_path, result, failure_reason, bytes_transferred, duration_ms, "
    "user_agent, request_id";

void sendJsonError(std::function<void(const drogon::HttpResponsePtr &)> &callback,
                    drogon::HttpStatusCode code, const std::string &msg) {
    Json::Value err; err["error"] = msg;
    auto r = drogon::HttpResponse::newHttpJsonResponse(err);
    r->setStatusCode(code);
    callback(r);
}

} // namespace

// ── GET /api/events ──────────────────────────────────────────────────────────

void EventsController::list(const drogon::HttpRequestPtr &req,
                             std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    sqlite3 *db = openReadOnly(dbPathFromConfig());
    if (!db) {
        return sendJsonError(callback, drogon::k500InternalServerError,
                              "Event log unavailable");
    }

    std::string type   = req->getParameter("type");
    std::string user   = req->getParameter("user");
    std::string result = req->getParameter("result");
    std::string from   = req->getParameter("from"); // ISO 8601, inclusive
    std::string to     = req->getParameter("to");   // ISO 8601, exclusive

    int limit = 100, offset = 0;
    if (!req->getParameter("limit").empty())
        limit = std::min(1000, std::max(1, std::stoi(req->getParameter("limit"))));
    if (!req->getParameter("offset").empty())
        offset = std::max(0, std::stoi(req->getParameter("offset")));

    std::ostringstream sql;
    sql << "SELECT " << kSelectColumns << " FROM nas_events WHERE 1=1";
    std::vector<std::string> binds;

    if (!type.empty())   { sql << " AND event_type = ?";    binds.push_back(type); }
    if (!user.empty())   { sql << " AND actor_user = ?";    binds.push_back(user); }
    if (!result.empty()) { sql << " AND result = ?";        binds.push_back(result); }
    if (!from.empty())   { sql << " AND timestamp_utc >= ?"; binds.push_back(from); }
    if (!to.empty())     { sql << " AND timestamp_utc < ?";  binds.push_back(to); }

    sql << " ORDER BY id DESC LIMIT ? OFFSET ?";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return sendJsonError(callback, drogon::k500InternalServerError, "Query failed");
    }

    int idx = 1;
    for (auto &b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);

    Json::Value items(Json::arrayValue);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        items.append(rowToJson(stmt));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    Json::Value result_;
    result_["items"] = items;
    result_["limit"] = limit;
    result_["offset"] = offset;
    callback(drogon::HttpResponse::newHttpJsonResponse(result_));
}

// ── GET /api/events/:id ───────────────────────────────────────────────────────

void EventsController::get(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                            int id) {
    sqlite3 *db = openReadOnly(dbPathFromConfig());
    if (!db) {
        return sendJsonError(callback, drogon::k500InternalServerError,
                              "Event log unavailable");
    }

    std::string sql = std::string("SELECT ") + kSelectColumns +
                       " FROM nas_events WHERE id = ?";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return sendJsonError(callback, drogon::k500InternalServerError, "Query failed");
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
    sendJsonError(callback, drogon::k404NotFound, "Event not found");
}

// ── GET /api/events/export ────────────────────────────────────────────────────

void EventsController::exportCsv(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    sqlite3 *db = openReadOnly(dbPathFromConfig());
    if (!db) {
        return sendJsonError(callback, drogon::k500InternalServerError,
                              "Event log unavailable");
    }

    std::string from = req->getParameter("from");
    std::string to   = req->getParameter("to");

    std::ostringstream sql;
    sql << "SELECT " << kSelectColumns << " FROM nas_events WHERE 1=1";
    std::vector<std::string> binds;
    if (!from.empty()) { sql << " AND timestamp_utc >= ?"; binds.push_back(from); }
    if (!to.empty())   { sql << " AND timestamp_utc < ?";  binds.push_back(to); }
    sql << " ORDER BY id ASC";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return sendJsonError(callback, drogon::k500InternalServerError, "Query failed");
    }
    int idx = 1;
    for (auto &b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);

    std::ostringstream csv;
    csv << "id,timestamp_utc,event_type,actor_user,source_ip,target_path,"
           "secondary_path,result,failure_reason,bytes_transferred,duration_ms,"
           "user_agent,request_id\n";

    auto csvField = [](const char *raw) -> std::string {
        if (!raw) return "";
        std::string s(raw);
        bool needsQuote = s.find_first_of(",\"\n") != std::string::npos;
        if (!needsQuote) return s;
        std::string out = "\"";
        for (char c : s) { if (c == '"') out += '"'; out += c; }
        out += "\"";
        return out;
    };

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        csv << sqlite3_column_int(stmt, 0) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 1)) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 2)) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 3)) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 4)) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 5)) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 6)) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 7)) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 8)) << ",";
        if (sqlite3_column_type(stmt, 9) != SQLITE_NULL)
            csv << sqlite3_column_int64(stmt, 9);
        csv << ",";
        if (sqlite3_column_type(stmt, 10) != SQLITE_NULL)
            csv << sqlite3_column_int64(stmt, 10);
        csv << ","
            << csvField((const char *)sqlite3_column_text(stmt, 11)) << ","
            << csvField((const char *)sqlite3_column_text(stmt, 12)) << "\n";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    resp->addHeader("Content-Disposition", "attachment; filename=\"events_export.csv\"");
    resp->setBody(csv.str());
    callback(resp);
}

// ── GET /api/events/stats/summary ─────────────────────────────────────────────
// Default (no group_by): counts by event_type × result.
// group_by=actor:     top actors by event count in window.
// group_by=source_ip: top source IPs by event count in window.
// Optional: from=, to= (ISO 8601), limit= (default 10, max 50).

void EventsController::summary(const drogon::HttpRequestPtr &req,
                                std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    sqlite3 *db = openReadOnly(dbPathFromConfig());
    if (!db) {
        return sendJsonError(callback, drogon::k500InternalServerError,
                              "Event log unavailable");
    }

    std::string from    = req->getParameter("from");
    std::string to      = req->getParameter("to");
    std::string groupBy = req->getParameter("group_by"); // "actor" | "source_ip" | ""

    // ── group_by branch ───────────────────────────────────────────────────────
    if (groupBy == "actor" || groupBy == "source_ip") {
        // Whitelist the column name — never interpolate user input directly
        const char *col = (groupBy == "actor") ? "actor_user" : "source_ip";

        int limit = 10;
        if (!req->getParameter("limit").empty())
            limit = std::min(50, std::max(1, std::stoi(req->getParameter("limit"))));

        std::ostringstream sql;
        sql << "SELECT " << col << ", COUNT(*) as total, "
            << "SUM(CASE WHEN result='failure' THEN 1 ELSE 0 END) as failures "
            << "FROM nas_events WHERE " << col << " IS NOT NULL";

        std::vector<std::string> binds;
        if (!from.empty()) { sql << " AND timestamp_utc >= ?"; binds.push_back(from); }
        if (!to.empty())   { sql << " AND timestamp_utc < ?";  binds.push_back(to); }
        sql << " GROUP BY " << col << " ORDER BY total DESC LIMIT ?";

        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return sendJsonError(callback, drogon::k500InternalServerError, "Query failed");
        }
        int idx = 1;
        for (auto &b : binds)
            sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, idx++, limit);

        Json::Value items(Json::arrayValue);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Json::Value row;
            const unsigned char *key = sqlite3_column_text(stmt, 0);
            row[groupBy]   = key ? (const char *)key : "";
            row["total"]   = (Json::Int64)sqlite3_column_int64(stmt, 1);
            row["failures"]= (Json::Int64)sqlite3_column_int64(stmt, 2);
            items.append(row);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);

        Json::Value out;
        out["group_by"] = groupBy;
        out["items"]    = items;
        callback(drogon::HttpResponse::newHttpJsonResponse(out));
        return;
    }

    // ── default branch: by event_type × result ────────────────────────────────
    std::ostringstream sql;
    sql << "SELECT event_type, result, COUNT(*) FROM nas_events WHERE 1=1";
    std::vector<std::string> binds;
    if (!from.empty()) { sql << " AND timestamp_utc >= ?"; binds.push_back(from); }
    if (!to.empty())   { sql << " AND timestamp_utc < ?";  binds.push_back(to); }
    sql << " GROUP BY event_type, result";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return sendJsonError(callback, drogon::k500InternalServerError, "Query failed");
    }
    int idx = 1;
    for (auto &b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);

    Json::Value byType(Json::objectValue);
    int64_t totalSuccess = 0, totalFailure = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string type   = (const char *)sqlite3_column_text(stmt, 0);
        std::string result = (const char *)sqlite3_column_text(stmt, 1);
        int64_t count      = sqlite3_column_int64(stmt, 2);

        if (!byType.isMember(type)) {
            byType[type]["success"] = 0;
            byType[type]["failure"] = 0;
        }
        byType[type][result] = (Json::Int64)count;

        if (result == "success") totalSuccess += count;
        else totalFailure += count;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    Json::Value out;
    out["by_type"]       = byType;
    out["total_success"] = (Json::Int64)totalSuccess;
    out["total_failure"] = (Json::Int64)totalFailure;
    callback(drogon::HttpResponse::newHttpJsonResponse(out));
}
