#include "UploadController.h"
#include "../services/EventRecorder.h"
#include <drogon/drogon.h>
#include <drogon/MultiPart.h>
#include <filesystem>
#include <json/json.h>

namespace fs = std::filesystem;

void UploadController::upload(const drogon::HttpRequestPtr &req,
                               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    std::string destRel = req->getParameter("path");
    if (destRel.empty()) destRel = "";
    std::string actor = req->getAttributes()->get<std::string>("username");

    auto &cfg = drogon::app().getCustomConfig();
    fs::path root = cfg.get("nas_root", "/home/noobiegg/nas/nas_storage").asString();
    root = fs::weakly_canonical(root);

    // Strip leading slash before joining
    while (!destRel.empty() && destRel[0] == '/') destRel = destRel.substr(1);

    fs::path destFull = fs::weakly_canonical(root / destRel);

    // Path traversal guard
    auto [rootEnd, _] = std::mismatch(root.begin(), root.end(), destFull.begin());
    if (rootEnd != root.end()) {
        EventRecorder::emit(NasEvent(EventType::FileUpload, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(destRel)
            .withFailureReason("path_traversal_or_forbidden"));
        Json::Value err; err["error"] = "Forbidden path";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k403Forbidden);
        return callback(r);
    }

    if (!fs::is_directory(destFull)) {
        Json::Value err; err["error"] = "Destination is not a directory";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        return callback(r);
    }

    drogon::MultiPartParser parser;
    if (parser.parse(req) != 0) {
        Json::Value err; err["error"] = "Failed to parse multipart body";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        return callback(r);
    }

    auto &files = parser.getFiles();
    if (files.empty()) {
        Json::Value err; err["error"] = "No files uploaded";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        return callback(r);
    }

    Json::Value saved(Json::arrayValue);
    for (auto &f : files) {
        fs::path dest = destFull / f.getFileName();
        f.saveAs(dest.string());
        saved.append(f.getFileName());

        // One event per file, matching the granularity of every other event
        // type (one row per filesystem object touched, not per HTTP request).
        EventRecorder::emit(NasEvent(EventType::FileUpload, EventResult::Success)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath((fs::path(destRel) / f.getFileName()).string())
            .withBytesTransferred((int64_t)f.fileLength()));
    }

    Json::Value result;
    result["saved"] = saved;
    callback(drogon::HttpResponse::newHttpJsonResponse(result));
}
