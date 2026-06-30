#pragma once
#include <drogon/HttpController.h>

// Sync status, logs, and mock control endpoints. Behind JwtFilter like every
// other controller — sync state (and especially the rotating port, even
// though it's not yet consumed by a real external service) is operational
// data that shouldn't be exposed unauthenticated, same reasoning as
// EventsController/AlertsController.
class SyncController : public drogon::HttpController<SyncController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(SyncController::status, "/api/sync/status", drogon::Get,  "JwtFilter");
        ADD_METHOD_TO(SyncController::logs,   "/api/sync/logs",   drogon::Get,  "JwtFilter");
        ADD_METHOD_TO(SyncController::start,  "/api/sync/start",  drogon::Post, "JwtFilter");
        ADD_METHOD_TO(SyncController::pause,  "/api/sync/pause",  drogon::Post, "JwtFilter");
        ADD_METHOD_TO(SyncController::resume, "/api/sync/resume", drogon::Post, "JwtFilter");
    METHOD_LIST_END

    // GET /api/sync/status — state, percent/eta (mocked), hash, current port
    void status(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // GET /api/sync/logs?limit= — recent sync log entries, most recent last
    void logs(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/sync/start — mock: begins a simulated sync run
    void start(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/sync/pause — mock: pauses an in-progress simulated sync
    void pause(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/sync/resume — mock: resumes a paused simulated sync
    void resume(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
