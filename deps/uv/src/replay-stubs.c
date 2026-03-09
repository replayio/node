/* Copyright 2020-2024 Record Replay Inc. */

/* The fork's libuv changes call into the record/replay driver, whose entry
 * points V8 defines (deps/v8/src/api/api.cc). Node 20 added host build tools
 * -- node_js2c -- that link libuv without V8, so those calls are unresolved
 * there. Define them weakly here: the linker prefers V8's strong definitions
 * when building node itself, and the tools get these no-ops, which is right
 * because a build tool never records or replays.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define WEAK __attribute__((weak))

WEAK int V8RecordReplayIsRecording(void) { return 0; }
WEAK int V8RecordReplayIsReplaying(void) { return 0; }
WEAK int V8RecordReplayHasAsserts(void) { return 0; }
WEAK int V8RecordReplayPointerId(const void* ptr) { (void)ptr; return 0; }
WEAK void* V8RecordReplayIdPointer(int id) { (void)id; return NULL; }
WEAK size_t V8RecordReplayCreateOrderedLock(const char* name) {
  (void)name;
  return 0;
}
WEAK uintptr_t V8RecordReplayValue(const char* why, uintptr_t value) {
  (void)why;
  return value;
}
WEAK void V8RecordReplayAssert(const char* format, ...) { (void)format; }
WEAK void V8RecordReplayAssertMaybeEventsDisallowed(const char* format, ...) {
  (void)format;
}
WEAK void V8RecordReplayDiagnostic(const char* format, ...) { (void)format; }
WEAK void V8RecordReplayBeginPassThroughEvents(void) {}
WEAK void V8RecordReplayEndPassThroughEvents(void) {}
WEAK void V8RecordReplayOrderedLock(int lock) { (void)lock; }
WEAK void V8RecordReplayOrderedUnlock(int lock) { (void)lock; }
WEAK void V8RecordReplayRegisterPointer(const void* ptr) { (void)ptr; }
WEAK void V8RecordReplayAddOrderedPthreadMutex(const char* name,
                                               pthread_mutex_t* mutex) {
  (void)name;
  (void)mutex;
}

#undef WEAK
