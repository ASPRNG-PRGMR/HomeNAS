#include "EventRecorder.h"
#include "EventWriter.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {

// ISO 8601 UTC, e.g. "2026-06-20T14:03:11Z" — matches schema_events.sql comment.
std::string nowIso8601Utc() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace

void EventRecorder::emit(NasEvent event) {
    // Timestamp is stamped here, at the moment the controller observed the
    // outcome — not later when the writer thread gets around to it — so
    // ordering and accuracy hold even if the queue is briefly backed up.
    EventWriter::instance().enqueue(std::move(event), nowIso8601Utc());
}
