#include "FilesystemController.h"
#include "../services/EventRecorder.h"
#include <drogon/drogon.h>
#include <filesystem>
#include <json/json.h>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ── path safety ──────────────────────────────────────────────────────────────

std::string FilesystemController::safePath(const std::string &rel) {
    auto &cfg = drogon::app().getCustomConfig();
    fs::path root = cfg.get("nas_root", "/home/noobiegg/nas/nas_storage").asString();
    root = fs::weakly_canonical(root);

    // Strip leading slash so joining works correctly
    std::string clean = rel;
    while (!clean.empty() && clean[0] == '/') clean = clean.substr(1);

    fs::path full = fs::weakly_canonical(root / clean);

    // Reject path traversal
    auto [rootEnd, nothing] = std::mismatch(root.begin(), root.end(), full.begin());
    if (rootEnd != root.end()) return "";

    return full.string();
}

// ── list directory ────────────────────────────────────────────────────────────

void FilesystemController::list(const drogon::HttpRequestPtr &req,
                                 std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    std::string rel = req->getParameter("path");
    if (rel.empty()) rel = "/";

    std::string full = safePath(rel);
    if (full.empty()) {
        Json::Value err; err["error"] = "Forbidden path";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k403Forbidden);
        return callback(r);
    }

    if (!fs::exists(full) || !fs::is_directory(full)) {
        Json::Value err; err["error"] = "Not a directory";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k404NotFound);
        return callback(r);
    }

    Json::Value items(Json::arrayValue);
    for (auto &entry : fs::directory_iterator(full)) {
    	if (entry.path().filename() == ".nas-meta") 
	    continue;
	
        Json::Value item;
        item["name"] = entry.path().filename().string();
        item["path"] = fs::relative(entry.path(),
            drogon::app().getCustomConfig().get("nas_root", "/home/noobiegg/nas/nas_storage").asString()).string();
        item["is_dir"] = entry.is_directory();
        if (!entry.is_directory()) {
            item["size"] = (Json::Int64)entry.file_size();
        }
        auto t = entry.last_write_time();
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        item["modified"] = (Json::Int64)std::chrono::system_clock::to_time_t(sctp);
        items.append(item);
    }

    Json::Value result;
    result["path"] = rel;
    result["items"] = items;
    callback(drogon::HttpResponse::newHttpJsonResponse(result));
}

// ── download file ─────────────────────────────────────────────────────────────

void FilesystemController::get(const drogon::HttpRequestPtr &req,
                                std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    std::string rel = req->getParameter("path");
    std::string full = safePath(rel);
    std::string actor = req->getAttributes()->get<std::string>("username");

    if (full.empty()) {
        EventRecorder::emit(NasEvent(EventType::FileDownload, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(rel)
            .withFailureReason("path_traversal_or_forbidden"));
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k403Forbidden);
        return callback(r);
    }

    if (!fs::exists(full) || fs::is_directory(full)) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k404NotFound);
        return callback(r);
    }

    std::error_code ec;
    auto fileSize = fs::file_size(full, ec);
    std::optional<int64_t> sizeOpt;
    if (!ec) sizeOpt = (int64_t)fileSize;

    EventRecorder::emit(NasEvent(EventType::FileDownload, EventResult::Success)
        .withActor(actor)
        .withSourceIp(req->getPeerAddr().toIp())
        .withTargetPath(rel)
        .withBytesTransferred(sizeOpt));

    auto resp = drogon::HttpResponse::newFileResponse(full);
    resp->addHeader("Content-Disposition",
        "attachment; filename=\"" + fs::path(full).filename().string() + "\"");
    callback(resp);
}

// ── delete ────────────────────────────────────────────────────────────────────

void FilesystemController::remove(const drogon::HttpRequestPtr &req,
                                   std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    std::string rel = req->getParameter("path");
    std::string full = safePath(rel);
    std::string actor = req->getAttributes()->get<std::string>("username");

    if (full.empty()) {
        EventRecorder::emit(NasEvent(EventType::FileDelete, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(rel)
            .withFailureReason("path_traversal_or_forbidden"));
        Json::Value err; err["error"] = "Forbidden";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k403Forbidden);
        return callback(r);
    }

    // Determine file vs directory *before* deletion — the path won't exist
    // to check afterward. Drives which event_type we record.
    bool wasDirectory = fs::is_directory(full);
    EventType evType = wasDirectory ? EventType::DirDelete : EventType::FileDelete;

    std::error_code ec;
    fs::remove_all(full, ec);
    if (ec) {
        EventRecorder::emit(NasEvent(evType, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(rel)
            .withFailureReason(ec.message()));
        Json::Value err; err["error"] = ec.message();
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k500InternalServerError);
        return callback(r);
    }

    EventRecorder::emit(NasEvent(evType, EventResult::Success)
        .withActor(actor)
        .withSourceIp(req->getPeerAddr().toIp())
        .withTargetPath(rel));

    Json::Value ok; ok["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(ok));
}

// ── mkdir ─────────────────────────────────────────────────────────────────────

void FilesystemController::mkdir(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value err; err["error"] = "Bad request";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        return callback(r);
    }

    std::string rel = (*body)["path"].asString();
    std::string full = safePath(rel);
    std::string actor = req->getAttributes()->get<std::string>("username");

    if (full.empty()) {
        EventRecorder::emit(NasEvent(EventType::DirCreate, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(rel)
            .withFailureReason("path_traversal_or_forbidden"));
        Json::Value err; err["error"] = "Forbidden";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k403Forbidden);
        return callback(r);
    }

    std::error_code ec;
    fs::create_directories(full, ec);
    if (ec) {
        EventRecorder::emit(NasEvent(EventType::DirCreate, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(rel)
            .withFailureReason(ec.message()));
        Json::Value err; err["error"] = ec.message();
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k500InternalServerError);
        return callback(r);
    }

    EventRecorder::emit(NasEvent(EventType::DirCreate, EventResult::Success)
        .withActor(actor)
        .withSourceIp(req->getPeerAddr().toIp())
        .withTargetPath(rel));

    Json::Value ok; ok["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(ok));
}

// ── rename ────────────────────────────────────────────────────────────────────

void FilesystemController::rename(const drogon::HttpRequestPtr &req,
                                   std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value err; err["error"] = "Bad request";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k400BadRequest);
        return callback(r);
    }

    std::string fromRel = (*body)["from"].asString();
    std::string toRel   = (*body)["to"].asString();
    std::string from = safePath(fromRel);
    std::string to   = safePath(toRel);
    std::string actor = req->getAttributes()->get<std::string>("username");

    // No separate /api/move endpoint exists — this one handler covers both.
    // Distinguish them for logging by comparing parent directories: same
    // parent = rename in place, different parent = move. Heuristic, not
    // exact (e.g. a same-directory "rename" that also changes case on a
    // case-insensitive filesystem still counts as a rename here), but it
    // matches user intent closely enough without adding a new API surface.
    bool isMove = fs::path(fromRel).parent_path() != fs::path(toRel).parent_path();
    EventType evType = isMove ? EventType::FileMove : EventType::FileRename;

    if (from.empty() || to.empty()) {
        EventRecorder::emit(NasEvent(evType, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(fromRel)
            .withSecondaryPath(toRel)
            .withFailureReason("path_traversal_or_forbidden"));
        Json::Value err; err["error"] = "Forbidden";
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k403Forbidden);
        return callback(r);
    }

    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) {
        EventRecorder::emit(NasEvent(evType, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(fromRel)
            .withSecondaryPath(toRel)
            .withFailureReason(ec.message()));
        Json::Value err; err["error"] = ec.message();
        auto r = drogon::HttpResponse::newHttpJsonResponse(err);
        r->setStatusCode(drogon::k500InternalServerError);
        return callback(r);
    }

    EventRecorder::emit(NasEvent(evType, EventResult::Success)
        .withActor(actor)
        .withSourceIp(req->getPeerAddr().toIp())
        .withTargetPath(fromRel)
        .withSecondaryPath(toRel));

    Json::Value ok; ok["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(ok));
}
