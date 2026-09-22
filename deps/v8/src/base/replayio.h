// Copyright (c) 2024 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_BASE_REPLAYIO_H
#define V8_BASE_REPLAYIO_H

#include "include/replayio.h"

#include "src/base/optional.h"

namespace v8 {
namespace replayio {

inline bool CheckReplayOwned() {
  return v8::recordreplay::IsInReplayCode();
}

// Marks nested code as replay-owned (IsInReplayCode). Independent of
// AutoMaybeDisallowEvents; divergent replay-owned paths use both.
struct AutoMaybeMarkReplayCode {
  explicit AutoMaybeMarkReplayCode(bool markReplayCode) {
    if (markReplayCode) {
      mark.emplace();
    }
  }

 private:
  v8::base::Optional<v8::replayio::AutoMarkReplayCode> mark;
};

struct AutoMaybeDisallowEvents {
  AutoMaybeDisallowEvents(bool disallowEvents, v8::Isolate* isolate,
                          const char* label) {
    if (disallowEvents) {
      disallow.emplace(label, isolate);
    }
  }

 private:
  v8::base::Optional<v8::replayio::AutoDisallowEvents> disallow;
};

// Mark+Disallow for replay-owned inspector object work sites.
struct AutoMaybeReplayOwned {
  AutoMaybeReplayOwned(bool owned, v8::Isolate* isolate, const char* label)
      : mark(owned), disallow(owned, isolate, label) {}

 private:
  AutoMaybeMarkReplayCode mark;
  AutoMaybeDisallowEvents disallow;
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_BASE_REPLAYIO_H
