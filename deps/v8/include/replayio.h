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

}  // namespace replayio
}  // namespace v8

#endif  // INCLUDE_RECORD_REPLAY_H_
