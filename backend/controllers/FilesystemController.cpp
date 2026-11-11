#include "FilesystemController.h"
#include "../services/EventRecorder.h"
#include "../services/OrderStore.h"
#include <drogon/drogon.h>
#include <algorithm>
#include <filesystem>
#include <vector>
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
    // Manual row order, if one was saved for this directory. Always present
    // (possibly empty) so the client doesn't need to probe for it.
    Json::Value order(Json::arrayValue);
    for (const auto &n : OrderStore::instance().get(OrderStore::normalize(rel)))
        order.append(n);
    result["order"] = order;
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

    OrderStore::instance().onRemoved(OrderStore::normalize(rel));

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

    // fs::rename() silently replaces an existing file (and an existing empty
    // directory). Dragging a file into a folder that already holds one of the
    // same name would destroy that file with no warning and no way back, so
    // refuse instead. `from` and `to` naming the same entry — a case-only
    // rename on a case-insensitive filesystem — is not a conflict.
    {
        std::error_code probe;
        bool destExists = fs::exists(fs::symlink_status(to, probe));
        if (destExists && !fs::equivalent(from, to, probe)) {
            EventRecorder::emit(NasEvent(evType, EventResult::Failure)
                .withActor(actor)
                .withSourceIp(req->getPeerAddr().toIp())
                .withTargetPath(fromRel)
                .withSecondaryPath(toRel)
                .withFailureReason("destination_exists"));
            Json::Value err;
            err["error"] = "An item with that name already exists there";
            auto r = drogon::HttpResponse::newHttpJsonResponse(err);
            r->setStatusCode(drogon::k409Conflict);
            return callback(r);
        }
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

    OrderStore::instance().onRenamed(OrderStore::normalize(fromRel),
                                     OrderStore::normalize(toRel));

    EventRecorder::emit(NasEvent(evType, EventResult::Success)
        .withActor(actor)
        .withSourceIp(req->getPeerAddr().toIp())
        .withTargetPath(fromRel)
        .withSecondaryPath(toRel));

    Json::Value ok; ok["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(ok));
}

// ── merge ─────────────────────────────────────────────────────────────────────

namespace {

Json::Value errorJson(const std::string &msg) {
    Json::Value e; e["error"] = msg; return e;
}

bool touchesMeta(const std::string &rel) {
    for (const auto &part : fs::path(rel))
        if (part == ".nas-meta") return true;
    return false;
}

// "report.pdf" -> "report (2).pdf", "report (3).pdf", ... — the first one
// that isn't taken in `dir`.
fs::path freeName(const fs::path &dir, const fs::path &name) {
    const std::string stem = name.stem().string(), ext = name.extension().string();
    for (int n = 2; ; ++n) {
        fs::path candidate = dir / (stem + " (" + std::to_string(n) + ")" + ext);
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(candidate, ec))) return candidate;
    }
}

struct MergeStats {
    int moved = 0;                       // entries relocated (a moved subtree counts once)
    Json::Value renamed{Json::arrayValue}; // conflicts kept under a new name
};

// Moves the contents of `src` into `dst`. Rules, in order:
//   - nothing of that name in dst        -> move it across
//   - a directory on both sides          -> recurse, so the two trees fold together
//   - anything else (file vs file, file vs directory)
//                                        -> keep both; the incoming one gets " (2)"
// Nothing is ever overwritten or deleted except directories this has just
// emptied. On the first error it stops and leaves the rest of `src` where it
// was, so a failed merge is incomplete rather than lossy.
bool mergeInto(const fs::path &src, const fs::path &dst, const fs::path &topSrc,
               MergeStats &stats, std::error_code &ec) {
    // Snapshot first: renaming entries out of a directory while iterating it
    // is unspecified.
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
        entries.push_back(it->path());
    if (ec) return false;

    for (const auto &entry : entries) {
        fs::path target = dst / entry.filename();
        std::error_code probe;
        const auto targetStatus = fs::symlink_status(target, probe);

        if (!fs::exists(targetStatus)) {
            fs::rename(entry, target, ec);
            if (ec) return false;
            ++stats.moved;
            continue;
        }

        const bool bothDirs = fs::is_directory(targetStatus) &&
                              fs::is_directory(fs::symlink_status(entry, probe));
        // Merging a folder up into its own parent can meet a child that
        // shares the folder's name (A/A into A's parent): the "existing"
        // target is then the folder being emptied, not a real counterpart.
        const bool targetIsSource = fs::equivalent(target, topSrc, probe);

        if (bothDirs && !targetIsSource) {
            if (!mergeInto(entry, target, topSrc, stats, ec)) return false;
            fs::remove(entry, ec);          // non-recursive: only succeeds if emptied
            if (ec) return false;
            continue;
        }

        fs::path renamedTo = freeName(dst, entry.filename());
        fs::rename(entry, renamedTo, ec);
        if (ec) return false;
        ++stats.moved;
        Json::Value r;
        r["name"] = entry.filename().string();
        r["as"]   = renamedTo.filename().string();
        stats.renamed.append(r);
    }
    return true;
}

} // namespace

void FilesystemController::merge(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    auto reply = [&callback](drogon::HttpStatusCode code, const Json::Value &body) {
        auto r = drogon::HttpResponse::newHttpJsonResponse(body);
        r->setStatusCode(code);
        callback(r);
    };

    auto body = req->getJsonObject();
    if (!body) return reply(drogon::k400BadRequest, errorJson("Bad request"));

    std::string fromRel = (*body)["from"].asString();
    std::string toRel   = (*body)["to"].asString();
    std::string from = safePath(fromRel);
    std::string to   = safePath(toRel);
    std::string actor = req->getAttributes()->get<std::string>("username");

    // Logged as a single file.move from the source folder to the destination
    // rather than one event per file: it is one user action, and a per-file
    // stream would trip the mass-delete rules on any sizeable folder.
    auto fail = [&](drogon::HttpStatusCode code, const std::string &reason,
                    const std::string &message) {
        EventRecorder::emit(NasEvent(EventType::FileMove, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(fromRel)
            .withSecondaryPath(toRel)
            .withFailureReason(reason));
        reply(code, errorJson(message));
    };

    if (from.empty() || to.empty() || touchesMeta(fromRel) || touchesMeta(toRel))
        return fail(drogon::k403Forbidden, "path_traversal_or_forbidden", "Forbidden");

    std::error_code ec;
    if (!fs::is_directory(from, ec) || !fs::is_directory(to, ec))
        return fail(drogon::k400BadRequest, "not_a_directory",
                    "Both paths must be existing folders");
    if (fs::equivalent(from, to, ec))
        return fail(drogon::k400BadRequest, "same_directory",
                    "Can't merge a folder into itself");
    if (fs::equivalent(from, safePath("/"), ec))
        return fail(drogon::k400BadRequest, "source_is_root",
                    "Can't merge the top-level folder into another");
    // Destination inside the source would have the merge feeding on itself.
    {
        fs::path f = fs::path(from), t = fs::path(to);
        auto mm = std::mismatch(f.begin(), f.end(), t.begin(), t.end());
        if (mm.first == f.end())
            return fail(drogon::k400BadRequest, "destination_inside_source",
                        "Can't merge a folder into one of its own subfolders");
    }

    MergeStats stats;
    bool ok = mergeInto(from, to, from, stats, ec);
    if (ok) fs::remove(from, ec);           // now empty
    if (!ok || ec) {
        const std::string why = ec ? ec.message() : "merge failed";
        EventRecorder::emit(NasEvent(EventType::FileMove, EventResult::Failure)
            .withActor(actor)
            .withSourceIp(req->getPeerAddr().toIp())
            .withTargetPath(fromRel)
            .withSecondaryPath(toRel)
            .withFailureReason(why));
        Json::Value err = errorJson(why);
        // Tell the client how far it got: whatever is still in the source
        // folder was not moved.
        err["moved"]   = stats.moved;
        err["renamed"] = stats.renamed;
        return reply(drogon::k500InternalServerError, err);
    }

    OrderStore::instance().onRemoved(OrderStore::normalize(fromRel));

    EventRecorder::emit(NasEvent(EventType::FileMove, EventResult::Success)
        .withActor(actor)
        .withSourceIp(req->getPeerAddr().toIp())
        .withTargetPath(fromRel)
        .withSecondaryPath(toRel));

    Json::Value out;
    out["ok"]      = true;
    out["moved"]   = stats.moved;
    out["renamed"] = stats.renamed;
    reply(drogon::k200OK, out);
}

// ── manual order ──────────────────────────────────────────────────────────────

void FilesystemController::order(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    auto reply = [&callback](drogon::HttpStatusCode code, const Json::Value &body) {
        auto r = drogon::HttpResponse::newHttpJsonResponse(body);
        r->setStatusCode(code);
        callback(r);
    };

    auto body = req->getJsonObject();
    if (!body || !(*body)["order"].isArray())
        return reply(drogon::k400BadRequest, errorJson("Bad request"));

    std::string rel = (*body)["path"].asString();
    if (rel.empty()) rel = "/";
    std::string full = safePath(rel);
    if (full.empty() || touchesMeta(rel))
        return reply(drogon::k403Forbidden, errorJson("Forbidden path"));
    std::error_code ec;
    if (!fs::is_directory(full, ec))
        return reply(drogon::k404NotFound, errorJson("Not a directory"));

    // These are display names for one directory's own entries, nothing more:
    // reject anything that could be read as a path.
    const Json::Value &arr = (*body)["order"];
    if (arr.size() > 20000)
        return reply(drogon::k400BadRequest, errorJson("Too many entries"));
    std::vector<std::string> names;
    names.reserve(arr.size());
    for (const auto &v : arr) {
        std::string n = v.isString() ? v.asString() : "";
        if (n.empty() || n.size() > 255 || n == "." || n == ".." ||
            n.find('/') != std::string::npos || n.find('\0') != std::string::npos)
            return reply(drogon::k400BadRequest, errorJson("Invalid name in order"));
        names.push_back(std::move(n));
    }

    if (!OrderStore::instance().set(OrderStore::normalize(rel), names))
        return reply(drogon::k500InternalServerError, errorJson("Could not save order"));

    Json::Value ok; ok["ok"] = true;
    reply(drogon::k200OK, ok);
}
