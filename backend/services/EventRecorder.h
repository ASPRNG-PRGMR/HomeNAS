#pragma once
#include "EventTypes.h"

// EventRecorder is the only thing controllers talk to. emit() is cheap and
// non-blocking: it timestamps the event, pushes it onto EventWriter's queue,
// and returns immediately. The actual SQLite write happens later, in batches,
// on EventWriter's dedicated thread.
//
// Usage from a controller:
//   EventRecorder::emit(NasEvent(EventType::FileDelete, EventResult::Success)
//       .withActor(username)
//       .withSourceIp(req->getPeerAddr().toIp())
//       .withTargetPath(rel));

class EventRecorder {
public:
    // Fills in timestamp_utc, then hands off to EventWriter::enqueue().
    // Safe to call from any Drogon worker thread.
    static void emit(NasEvent event);
};
