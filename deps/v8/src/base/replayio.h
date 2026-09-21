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

// Whether user JS (an accessor, toString, Symbol.toPrimitive, a callback the
// program installed) must not run now: Replay's own code is running, so events
// are disallowed, and the replay hasn't diverged into a pause, where user JS
// may run because nothing it does reaches the replayed execution.
//
// Replay's own code doesn't run the same way when recording and replaying.
// Chromium installs Replay's inspector session only when replaying (unless
// the replay-only-command-handling feature is disabled), and the driver
// gathers console message contents, previews and frames (OnConsoleMessage)
// only when replaying. Node installs the session on both sides, but the
// session's work runs with events disallowed, so none of it is recorded.
// User JS run from there would change the program's state where the
// recording didn't, so Replay's session has to behave as if it weren't
// attached: describing a value for it skips anything that would run user JS.
// Sessions a user or the program attached (DevTools, Playwright, --inspect)
// run with events allowed, so they run user JS like upstream does, recorded
// and replayed.
//
// The alternative would be to embrace the session instead: install it on both
// sides and record its work, with events allowed, for everything it does at
// the same points when recording and replaying (describing console arguments,
// gathering console message data). The user JS it runs would then replay like
// any other. The cost is that every recording shows the program as if a
// debugger were attached: its accessors and toString run where they don't
// without one (anti-bot checks like Cloudflare Turnstile detect exactly
// that), and every console message pays for the descriptions when recording.
// Work queried later, on demand, would still have to run in a pause.
inline bool MustNotRunUserJS() {
  return v8::recordreplay::AreEventsDisallowed() &&
         !v8::recordreplay::HasDivergedFromRecording();
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
