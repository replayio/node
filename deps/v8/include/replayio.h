// Copyright (c) 2024 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// API for interacting with the record/replay driver.
// Some parts are still in v8.h and still need to be migrated.


#ifndef INCLUDE_RECORD_REPLAY_H_
#define INCLUDE_RECORD_REPLAY_H_

#include <vector>

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

// Suspends the isolate's GC poll while the embedder runs code the replay does
// not run, such as a binary module's own code in node (see
// napi_env__::CallIntoModule). The recording describes such code's effects as
// the calls it made into the embedder, which the replay performs at the same
// points instead of running the code: a poll reached from there records values
// the replay reads where it expects those calls, which leaves the recording
// unreplayable, and the cleanup it runs would run embedder code nested in
// whatever the module was doing. What the GC collected stays noted, and the
// next poll outside the scope describes it, the way a poll which a
// participant's own cleanup reaches is left to the next one.
class V8_EXPORT AutoSuspendReplayGCPoll {
 public:
  explicit AutoSuspendReplayGCPoll(Isolate* isolate);
  ~AutoSuspendReplayGCPoll();

  AutoSuspendReplayGCPoll(const AutoSuspendReplayGCPoll&) = delete;
  AutoSuspendReplayGCPoll& operator=(const AutoSuspendReplayGCPoll&) = delete;

 private:
  Isolate* isolate_;
  bool suspended_;
};

// Must be called at deterministic points, on the isolate's thread.
V8_EXPORT void AddReplayGCPollParticipant(Isolate* isolate,
                                          ReplayGCPollParticipant* participant);
V8_EXPORT void RemoveReplayGCPollParticipant(
    Isolate* isolate, ReplayGCPollParticipant* participant);

// A script compiled on the main thread while recording or replaying that names
// a source map, through a sourceMappingURL comment or the embedder's origin.
// The strings are those V8's inspector reports in Debugger.scriptParsed before
// the embedder turns the name into a URL; a missing one is empty.
struct SourceMappedScript {
  int id;
  Local<String> name;
  Local<String> source_url;
  Local<String> source_mapping_url;
};

// Every such script registered so far, by increasing id. Scripts are
// registered at the same points when recording and replaying, whether or not
// V8's debugger is active, so both sides see the same list.
V8_EXPORT std::vector<SourceMappedScript> GetSourceMappedScripts(
    Isolate* isolate);

}  // namespace replayio
}  // namespace v8

#endif  // INCLUDE_RECORD_REPLAY_H_
