#pragma once
#include <json/json.h>
#include <mutex>
#include <string>
#include <vector>

// Manual ordering of directory listings.
//
// The file manager lets you drag rows into an order of your own. That order
// has to live on the server rather than in the browser, or it would differ
// between the laptop and the phone looking at the same folder. It is a UI
// preference, not a filesystem action, so it goes in its own small JSON file
// beside the audit databases (.nas-meta/order.json) and is not recorded in
// the event log.
//
// Shape on disk:  { "<dir relative to nas_root>": ["name", "name", ...] }
// The root directory's key is the empty string.
//
// The lists are advisory. A name that no longer exists is simply skipped by
// the client, and an entry the list doesn't mention sorts after the ones it
// does — so a stale or partial list can never hide a file.
class OrderStore {
public:
    static OrderStore &instance();

    // Canonical key for a directory path as the API receives it ("/a/b/",
    // "a/b" and "/a/./b" are all "a/b"; the root is "").
    static std::string normalize(const std::string &rel);

    std::vector<std::string> get(const std::string &dirKey);

    // An empty list clears the directory's manual order. Returns false if
    // the store could not be written.
    bool set(const std::string &dirKey, const std::vector<std::string> &names);

    // Keep the store in step with the filesystem. Both take entry keys (the
    // thing renamed or removed), not their parent directories.
    void onRenamed(const std::string &fromKey, const std::string &toKey);
    void onRemoved(const std::string &key);

private:
    OrderStore() = default;
    void loadLocked();
    bool saveLocked();
    void dropFromParentLocked(const std::string &key);

    std::mutex  mu_;
    bool        loaded_ = false;
    std::string file_;          // empty = nas_root unknown, store disabled
    Json::Value data_{Json::objectValue};
};
