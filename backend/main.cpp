#include <drogon/drogon.h>
#include "controllers/AuthController.h"
#include "controllers/FilesystemController.h"
#include "controllers/UploadController.h"
#include "controllers/EventsController.h"
#include "controllers/AlertsController.h"
#include "services/EventWriter.h"
#include "services/AlertWriter.h"
#include "services/EventAnalyzer.h"

int main() {
    auto &app = drogon::app();
    app.loadConfigFile("/home/noobiegg/nas/nas_main/config.json");

    app.registerBeginningAdvice([&app]() {
        auto &cfg = app.getCustomConfig();

        std::string eventsDbPath = cfg.get(
            "events_db_path",
            "/home/noobiegg/nas/nas_storage/.nas-meta/events.db").asString();

        std::string alertsDbPath = cfg.get(
            "alerts_db_path",
            "/home/noobiegg/nas/nas_storage/.nas-meta/alerts.db").asString();

        // Order matters:
        // 1. EventWriter first — creates events.db and its schema
        // 2. AlertWriter second — applies the claimed_user migration to
        //    events.db (needs it to already exist), creates alerts.db
        // 3. EventAnalyzer last — opens a read-only handle to events.db
        //    (needs schema to already be applied)
        EventWriter::instance().init(eventsDbPath);
        AlertWriter::instance().init(alertsDbPath, eventsDbPath);
        EventAnalyzer::instance().init(eventsDbPath);
    });

    auto gracefulShutdown = []() {
        // Shutdown in reverse init order
        EventAnalyzer::instance().shutdown();
        AlertWriter::instance().shutdown();
        EventWriter::instance().shutdown();
        drogon::app().quit();
    };
    app.setTermSignalHandler(gracefulShutdown);
    app.setIntSignalHandler(gracefulShutdown);

    app.run();
}
