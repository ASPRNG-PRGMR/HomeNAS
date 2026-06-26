#pragma once
#include <drogon/HttpController.h>

class AlertsController : public drogon::HttpController<AlertsController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AlertsController::list,    "/api/alerts",               drogon::Get,   "JwtFilter");
        ADD_METHOD_TO(AlertsController::get,     "/api/alerts/{id}",          drogon::Get,   "JwtFilter");
        ADD_METHOD_TO(AlertsController::summary, "/api/alerts/stats/summary", drogon::Get,   "JwtFilter");
        ADD_METHOD_TO(AlertsController::update,  "/api/alerts/{id}/status",   drogon::Patch, "JwtFilter");
    METHOD_LIST_END

    // GET /api/alerts?severity=&rule=&status=&from=&to=&limit=&offset=
    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // GET /api/alerts/:id
    void get(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&callback,
             int id);

    // GET /api/alerts/stats/summary — counts by severity/status, for dashboard header
    void summary(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // PATCH /api/alerts/:id/status — body: {"status":"investigating"|"dismissed"}
    // The only write endpoint on alerts — operators mark alerts as investigated
    // or dismissed. No delete: alerts are permanent evidence, same as events.
    void update(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                int id);
};
