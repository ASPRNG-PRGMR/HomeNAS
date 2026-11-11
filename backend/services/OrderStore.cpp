#include "OrderStore.h"
#include <drogon/drogon.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

std::string parentOf(const std::string &key) {
    auto slash = key.rfind('/');
    return slash == std::string::npos ? "" : key.substr(0, slash);
}

std::string nameOf(const std::string &key) {
    auto slash = key.rfind('/');
    return slash == std::string::npos ? key : key.substr(slash + 1);
}

// True for `key` itself and for anything beneath it.
bool isAtOrUnder(const std::string &candidate, const std::string &key) {
    if (candidate == key) return true;
    return candidate.size() > key.size() &&
           candidate.compare(0, key.size(), key) == 0 &&
           candidate[key.size()] == '/';
}

} // namespace

OrderStore &OrderStore::instance() {
    static OrderStore s;
    return s;
}

std::string OrderStore::normalize(const std::string &rel) {
    std::string s = fs::path(rel).lexically_normal().generic_string();
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    while (!s.empty() && s.back()  == '/') s.pop_back();
    if (s == ".") s.clear();
    return s;
}

void OrderStore::loadLocked() {
    if (loaded_) return;
    loaded_ = true;

    std::string root = drogon::app().getCustomConfig().get("nas_root", "").asString();
    if (root.empty()) {
        LOG_WARN << "OrderStore: nas_root not configured — manual ordering disabled";
        return;
    }
    file_ = (fs::path(root) / ".nas-meta" / "order.json").string();

    std::ifstream in(file_);
    if (!in) return;                        // first run: nothing saved yet
    Json::Value parsed;
    Json::CharReaderBuilder rb;
    std::string errs;
    if (Json::parseFromStream(rb, in, &parsed, &errs) && parsed.isObject()) {
        data_ = parsed;
    } else {
        // Losing a display order is harmless; refusing to list files over it
        // would not be. Start empty and overwrite on the next save.
        LOG_WARN << "OrderStore: ignoring unreadable " << file_ << ": " << errs;
    }
}

bool OrderStore::saveLocked() {
    if (file_.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::path(file_).parent_path(), ec);

    // Write-then-rename so a crash mid-write leaves the old file, not half
    // of a new one.
    std::string tmp = file_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        out << Json::writeString(wb, data_);
        if (!out) return false;
    }
    fs::rename(tmp, file_, ec);
    return !ec;
}

std::vector<std::string> OrderStore::get(const std::string &dirKey) {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    std::vector<std::string> out;
    const Json::Value &arr = data_[dirKey];
    if (arr.isArray())
        for (const auto &v : arr)
            if (v.isString()) out.push_back(v.asString());
    return out;
}

bool OrderStore::set(const std::string &dirKey, const std::vector<std::string> &names) {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    if (names.empty()) {
        if (!data_.isMember(dirKey)) return true;
        data_.removeMember(dirKey);
    } else {
        Json::Value arr(Json::arrayValue);
        for (const auto &n : names) arr.append(n);
        data_[dirKey] = arr;
    }
    return saveLocked();
}

void OrderStore::dropFromParentLocked(const std::string &key) {
    const std::string parent = parentOf(key), name = nameOf(key);
    if (!data_.isMember(parent) || !data_[parent].isArray()) return;
    Json::Value kept(Json::arrayValue);
    for (const auto &v : data_[parent])
        if (!v.isString() || v.asString() != name) kept.append(v);
    if (kept.empty()) data_.removeMember(parent);
    else              data_[parent] = kept;
}

void OrderStore::onRenamed(const std::string &fromKey, const std::string &toKey) {
    if (fromKey.empty() || fromKey == toKey) return;
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    bool changed = false;

    // A directory takes its own order, and that of everything inside it,
    // along to the new path.
    for (const auto &k : data_.getMemberNames()) {
        if (!isAtOrUnder(k, fromKey)) continue;
        data_[toKey + k.substr(fromKey.size())] = data_[k];
        data_.removeMember(k);
        changed = true;
    }

    const std::string parent = parentOf(fromKey);
    if (data_.isMember(parent) && data_[parent].isArray()) {
        if (parent == parentOf(toKey)) {
            // Renamed in place: keep its slot in the list.
            for (auto &v : data_[parent])
                if (v.isString() && v.asString() == nameOf(fromKey)) {
                    v = nameOf(toKey);
                    changed = true;
                }
        } else {
            dropFromParentLocked(fromKey);
            changed = true;
        }
    }
    if (changed) saveLocked();
}

void OrderStore::onRemoved(const std::string &key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    for (const auto &k : data_.getMemberNames())
        if (isAtOrUnder(k, key)) data_.removeMember(k);
    dropFromParentLocked(key);
    saveLocked();
}
