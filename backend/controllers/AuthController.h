#pragma once
#include <drogon/HttpController.h>

class AuthController : public drogon::HttpController<AuthController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AuthController::login, "/api/auth/login", drogon::Post);
        ADD_METHOD_TO(AuthController::logout, "/api/auth/logout", drogon::Post);
    METHOD_LIST_END

    void login(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void logout(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // Utility: generate JWT for a username
    static std::string generateJwt(const std::string &username);

    // Utility: validate JWT, returns username or empty string
    static std::string validateJwt(const std::string &token);
};
