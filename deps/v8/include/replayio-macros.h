// Copyright (c) 2024 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// API for interacting with the record/replay driver.
// Some parts are still in v8.h and still need to be migrated.


#ifndef INCLUDE_REPLAYIO_MACROS_H_
#define INCLUDE_REPLAYIO_MACROS_H_

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

bool V8RecordReplayHasAsserts(void);
void V8RecordReplayAssert(const char* format, ...);
void V8RecordReplayAssertMaybeEventsDisallowed(const char* format, ...);

#ifdef __cplusplus
}  // extern "C"
#endif

// Use this to wrap Asserts on non-trivial data, to avoid the
// overhead of argument evaluation when Asserts are disabled.
#define REPLAY_ASSERT(format, ...) \
  if (V8RecordReplayHasAsserts()) \
    V8RecordReplayAssert(format, ##__VA_ARGS__); \
  ((void)0)

// Same as |REPLAY_ASSERT| but won't Assert when Events are disallowed.
#define REPLAY_ASSERT_MAYBE_EVENTS_DISALLOWED(format, ...) \
  if (V8RecordReplayHasAsserts()) \
    V8RecordReplayAssertMaybeEventsDisallowed(format, ##__VA_ARGS__); \
  ((void)0)

#endif  // INCLUDE_REPLAYIO_MACROS_H_
