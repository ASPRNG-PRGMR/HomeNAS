#include <drogon/drogon.h>
#include "controllers/AuthController.h"
#include "controllers/FilesystemController.h"
#include "controllers/UploadController.h"
#include "controllers/EventsController.h"
#include "controllers/AlertsController.h"
#include "controllers/SyncController.h"
#include "services/EventWriter.h"
#include "services/AlertWriter.h"
#include "services/EventAnalyzer.h"
#include "services/SyncManager.h"

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

        // SyncManager is independent of the events/alerts pipeline — it
        // walks nas_root directly rather than reading events.db, so it has
        // no ordering dependency on the three calls above. Placed last
        // simply to keep the audit-trail subsystem's init block together.
        std::string nasRoot = cfg.get(
            "nas_root", "/home/noobiegg/nas/nas_storage").asString();
        int hashIntervalSeconds = cfg["sync"].get(
            "hash_check_interval_seconds", 300).asInt();
        int portRotationIntervalSeconds = cfg["sync"].get(
            "port_rotation_interval_seconds", 86400).asInt();
        // Stopgap portal listener directory — see SyncManager's class doc
        // comment. Empty/missing disables the listener spawn entirely.
        std::string syncPortalDir = cfg["sync"].get(
            "portal_dir", "/home/noobiegg/nas/nas_main/sync-portal").asString();
        SyncManager::instance().init(
            nasRoot, hashIntervalSeconds, portRotationIntervalSeconds,
            syncPortalDir);
    });

    auto gracefulShutdown = []() {
        // Shutdown in reverse init order
        SyncManager::instance().shutdown();
        EventAnalyzer::instance().shutdown();
        AlertWriter::instance().shutdown();
        EventWriter::instance().shutdown();
        drogon::app().quit();
    };
    app.setTermSignalHandler(gracefulShutdown);
    app.setIntSignalHandler(gracefulShutdown);

    app.run();
}
