#pragma once
#include <drogon/HttpController.h>

class FilesystemController : public drogon::HttpController<FilesystemController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(FilesystemController::list,   "/api/ls",     drogon::Get,    "JwtFilter");
        ADD_METHOD_TO(FilesystemController::get,    "/api/file",   drogon::Get,    "JwtFilter");
        ADD_METHOD_TO(FilesystemController::remove, "/api/file",   drogon::Delete, "JwtFilter");
        ADD_METHOD_TO(FilesystemController::mkdir,  "/api/mkdir",  drogon::Post,   "JwtFilter");
        ADD_METHOD_TO(FilesystemController::rename, "/api/rename", drogon::Post,   "JwtFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void get(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void remove(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void mkdir(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void rename(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

private:
    // Resolve a user-supplied path against nas_root, reject traversal attempts
    static std::string safePath(const std::string &rel);
};
