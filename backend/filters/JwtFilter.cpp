#include "JwtFilter.h"
#include "../controllers/AuthController.h"
#include "../services/EventRecorder.h"
#include <drogon/drogon.h>
#include <json/json.h>

void JwtFilter::doFilter(const drogon::HttpRequestPtr &req,
                          drogon::FilterCallback    &&fcb,
                          drogon::FilterChainCallback &&fccb) {
    std::string auth = req->getHeader("Authorization");

    if (auth.empty() || auth.substr(0, 7) != "Bearer ") {
        EventRecorder::emit(NasEvent(EventType::AuthLoginFailure, EventResult::Failure)
            .withSourceIp(req->getPeerAddr().toIp())
            .withFailureReason("missing_or_malformed_auth_header")
            .withUserAgent(req->getHeader("User-Agent")));
        Json::Value err;
        err["error"] = "Missing or malformed Authorization header";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k401Unauthorized);
        fcb(resp);
        return;
    }

    std::string token = auth.substr(7);
    std::string username = AuthController::validateJwt(token);

    if (username.empty()) {
        EventRecorder::emit(NasEvent(EventType::AuthLoginFailure, EventResult::Failure)
            .withSourceIp(req->getPeerAddr().toIp())
            .withFailureReason("invalid_or_expired_token")
            .withUserAgent(req->getHeader("User-Agent")));
        Json::Value err;
        err["error"] = "Invalid or expired token";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k401Unauthorized);
        fcb(resp);
        return;
    }

    // Pass the username downstream via request attribute
    req->getAttributes()->insert("username", username);
    fccb();
}
