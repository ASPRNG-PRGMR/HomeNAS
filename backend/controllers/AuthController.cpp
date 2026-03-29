#include "AuthController.h"
#include <drogon/drogon.h>
#include <json/json.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <chrono>

// ── helpers ──────────────────────────────────────────────────────────────────

static std::string base64UrlEncode(const std::string &in) {
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    for (char &c : out) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    out.erase(std::remove(out.begin(), out.end(), '='), out.end());
    return out;
}

static std::string base64UrlDecode(const std::string &in) {
    std::string s = in;
    for (char &c : s) {
        if (c == '-') c = '+';
        if (c == '_') c = '/';
    }
    while (s.size() % 4) s += '=';
    std::string out;
    std::vector<int> T(256, -1);
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) T[(unsigned char)tbl[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : s) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) { out.push_back(char((val >> valb) & 0xFF)); valb -= 8; }
    }
    return out;
}

static std::string hmacSha256(const std::string &key, const std::string &data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), (int)key.size(),
         (unsigned char*)data.data(), (int)data.size(),
         digest, &len);
    return std::string((char*)digest, len);
}

// ── JWT ──────────────────────────────────────────────────────────────────────

std::string AuthController::generateJwt(const std::string &username) {
    auto &cfg = drogon::app().getCustomConfig();
    std::string secret = cfg.get("jwt_secret", "changeme").asString();
    int expiry = cfg.get("jwt_expiry_seconds", 86400).asInt();

    std::string header = base64UrlEncode(R"({"alg":"HS256","typ":"JWT"})");

    auto now = std::chrono::system_clock::now();
    long iat = std::chrono::duration_cast<std::chrono::seconds>(
                   now.time_since_epoch()).count();
    long exp = iat + expiry;

    Json::Value payload;
    payload["sub"] = username;
    payload["iat"] = (Json::Int64)iat;
    payload["exp"] = (Json::Int64)exp;
    Json::FastWriter w;
    std::string payloadJson = w.write(payload);
    payloadJson.erase(payloadJson.find_last_not_of("\n") + 1);

    std::string body = header + "." + base64UrlEncode(payloadJson);
    std::string sig  = base64UrlEncode(hmacSha256(secret, body));
    return body + "." + sig;
}

std::string AuthController::validateJwt(const std::string &token) {
    auto &cfg = drogon::app().getCustomConfig();
    std::string secret = cfg.get("jwt_secret", "changeme").asString();

    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos) return "";

    std::string body = token.substr(0, dot2);
    std::string sig  = token.substr(dot2 + 1);

    if (base64UrlEncode(hmacSha256(secret, body)) != sig) return "";

    std::string payloadJson = base64UrlDecode(token.substr(dot1 + 1, dot2 - dot1 - 1));
    Json::Value payload;
    Json::Reader reader;
    if (!reader.parse(payloadJson, payload)) return "";

    long exp = payload.get("exp", 0).asInt64();
    auto now = std::chrono::system_clock::now();
    long nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                      now.time_since_epoch()).count();
    if (nowSec > exp) return "";

    return payload.get("sub", "").asString();
}

// ── handlers ─────────────────────────────────────────────────────────────────

void AuthController::login(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value err;
        err["error"] = "Bad request: body is not valid JSON";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    std::string username = (*body)["username"].asString();
    std::string password = (*body)["password"].asString();

    auto &cfg = drogon::app().getCustomConfig();
    std::string cfgUser = cfg.get("admin_username", "admin").asString();
    std::string cfgPass = cfg.get("admin_password", "changeme").asString();

    // Debug: log what we received vs what config says
    LOG_INFO << "Login attempt - received user: [" << username << "] pass: [" << password << "]";
    LOG_INFO << "Config has    - user: [" << cfgUser << "] pass: [" << cfgPass << "]";

    if (username != cfgUser || password != cfgPass) {
        Json::Value err;
        err["error"] = "Invalid credentials";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k401Unauthorized);
        callback(resp);
        return;
    }

    std::string token = generateJwt(username);
    Json::Value result;
    result["token"] = token;
    callback(drogon::HttpResponse::newHttpJsonResponse(result));
}

void AuthController::logout(const drogon::HttpRequestPtr &req,
                             std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    Json::Value result;
    result["ok"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(result));
}
