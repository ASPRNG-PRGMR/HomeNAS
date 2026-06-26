#pragma once
#include <drogon/HttpController.h>

// Read-only access to the event log. Sits behind JwtFilter, same as every
// other controller — audit logs are themselves sensitive (they reveal file
// paths, usage patterns, failed-login source IPs) and must not be exposed
// unauthenticated.
class EventsController : public drogon::HttpController<EventsController> {
public:
    METHOD_LIST_BEGIN
        // Literal routes registered before the parameterized /{id} route —
        // otherwise "export" or "stats" could be matched as the {id}
        // placeholder depending on Drogon's route resolution order.
        ADD_METHOD_TO(EventsController::list,      "/api/events",               drogon::Get, "JwtFilter");
        ADD_METHOD_TO(EventsController::exportCsv, "/api/events/export",        drogon::Get, "JwtFilter");
        ADD_METHOD_TO(EventsController::summary,   "/api/events/stats/summary", drogon::Get, "JwtFilter");
        ADD_METHOD_TO(EventsController::get,       "/api/events/{id}",          drogon::Get, "JwtFilter");
    METHOD_LIST_END

    // GET /api/events?type=&user=&result=&from=&to=&limit=&offset=
    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // GET /api/events/:id
    void get(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&callback,
             int id);

    // GET /api/events/export?from=&to=&format=csv|json
    void exportCsv(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // GET /api/events/stats/summary?from=&to=
    void summary(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
