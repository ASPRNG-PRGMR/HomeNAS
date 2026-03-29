#pragma once
#include <drogon/HttpController.h>

class UploadController : public drogon::HttpController<UploadController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(UploadController::upload, "/api/upload", drogon::Post, "JwtFilter");
    METHOD_LIST_END

    void upload(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};
