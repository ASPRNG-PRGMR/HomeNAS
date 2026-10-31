#pragma once
#include <drogon/HttpController.h>

// Sync status, logs, run history, and control endpoints. Behind JwtFilter
// like every other controller — sync state is operational data that
// shouldn't be exposed unauthenticated, same reasoning as
// EventsController/AlertsController.
class SyncController : public drogon::HttpController<SyncController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(SyncController::status, "/api/sync/status", drogon::Get,  "JwtFilter");
        ADD_METHOD_TO(SyncController::logs,   "/api/sync/logs",   drogon::Get,  "JwtFilter");
        ADD_METHOD_TO(SyncController::start,  "/api/sync/start",  drogon::Post, "JwtFilter");
        ADD_METHOD_TO(SyncController::pause,  "/api/sync/pause",  drogon::Post, "JwtFilter");
        ADD_METHOD_TO(SyncController::resume, "/api/sync/resume", drogon::Post, "JwtFilter");
        ADD_METHOD_TO(SyncController::cancel, "/api/sync/cancel", drogon::Post, "JwtFilter");
        ADD_METHOD_TO(SyncController::runs,   "/api/sync/runs",   drogon::Get,  "JwtFilter");
    METHOD_LIST_END

    // GET /api/sync/status — state, live progress, hash, last/next run
    void status(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // GET /api/sync/logs?limit= — recent sync log entries, most recent last
    void logs(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/sync/start — queues a backup run on the worker thread
    void start(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/sync/pause — pauses the in-progress run between files
    void pause(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/sync/resume — resumes a paused run
    void resume(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/sync/cancel — abandons the in-progress run; files already
    // copied stay copied (the backup is additive, so a partial run is still
    // a valid, if incomplete, backup)
    void cancel(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // GET /api/sync/runs?limit= — finished run history, most recent first
    void runs(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
