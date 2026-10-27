// Copyright (c) 2024 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// API for interacting with the record/replay driver.
// Some parts are still in v8.h and still need to be migrated.


#ifndef INCLUDE_RECORD_REPLAY_H_
#define INCLUDE_RECORD_REPLAY_H_

#include "v8.h"

namespace v8 {
namespace replayio {

struct AutoPassThroughEvents {
  AutoPassThroughEvents() { v8::recordreplay::BeginPassThroughEvents(); }
  ~AutoPassThroughEvents() { v8::recordreplay::EndPassThroughEvents(); }
};

struct AutoMarkReplayCode {
  AutoMarkReplayCode() { v8::recordreplay::EnterReplayCode(); }
  ~AutoMarkReplayCode() { v8::recordreplay::ExitReplayCode(); }

  AutoMarkReplayCode(const AutoMarkReplayCode&) = delete;
  AutoMarkReplayCode& operator=(const AutoMarkReplayCode&) = delete;
};

struct AutoDisallowEvents {
  AutoDisallowEvents() { Begin(nullptr); }
  explicit AutoDisallowEvents(const char* label, v8::Isolate* = nullptr) {
    Begin(label);
  }
  ~AutoDisallowEvents() { v8::recordreplay::EndDisallowEvents(); }

  AutoDisallowEvents(const AutoDisallowEvents&) = delete;
  AutoDisallowEvents& operator=(const AutoDisallowEvents&) = delete;

 private:
  void Begin(const char* label) {
    if (label) {
      v8::recordreplay::BeginDisallowEventsWithLabel(label);
    } else {
      v8::recordreplay::BeginDisallowEvents();
    }
  }
};

// Lets the embedder describe what the GC did to its own objects at the same
// deterministic points as V8's WeakRefs and FinalizationRegistries, so that
// each of those points records a single value (see src/replay/gc-poll.h).
//
// The embedder's weak callbacks only note what the GC collected. When
// recording, the poll records whether an active participant has notes. Where
// one did, the poll calls Poll() on every active participant on both sides,
// which records/replays the notes and runs the cleanup.
class ReplayGCPollParticipant {
 public:
  virtual ~ReplayGCPollParticipant() = default;

  // Whether the poll records a value on this participant's behalf. Has to be
  // the same when recording and replaying.
  virtual bool IsActive() const = 0;
  // Only called when recording: whether anything was noted since the last
  // Poll().
  virtual bool HasPending() const = 0;
  // Called on every active participant, in the order they were added, after
  // V8's own work. Must not add or remove participants.
  virtual void Poll() = 0;
};

// Must be called at deterministic points, on the isolate's thread.
V8_EXPORT void AddReplayGCPollParticipant(Isolate* isolate,
                                          ReplayGCPollParticipant* participant);
V8_EXPORT void RemoveReplayGCPollParticipant(
    Isolate* isolate, ReplayGCPollParticipant* participant);

}  // namespace replayio
}  // namespace v8

#endif  // INCLUDE_RECORD_REPLAY_H_
