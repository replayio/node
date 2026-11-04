// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** \mainpage V8 API Reference Guide
 *
 * V8 is Google's open source JavaScript engine.
 *
 * This set of documents provides reference material generated from the
 * V8 header files in the include/ subdirectory.
 *
 * For other documentation see https://v8.dev/.
 */

#ifndef INCLUDE_V8_H_
#define INCLUDE_V8_H_

#include <stddef.h>
#include <stdint.h>
#include <string>

#include <initializer_list>
#include <memory>

#include "cppgc/common.h"
#include "v8-array-buffer.h"       // NOLINT(build/include_directory)
#include "v8-container.h"          // NOLINT(build/include_directory)
#include "v8-context.h"            // NOLINT(build/include_directory)
#include "v8-data.h"               // NOLINT(build/include_directory)
#include "v8-date.h"               // NOLINT(build/include_directory)
#include "v8-debug.h"              // NOLINT(build/include_directory)
#include "v8-exception.h"          // NOLINT(build/include_directory)
#include "v8-extension.h"          // NOLINT(build/include_directory)
#include "v8-external.h"           // NOLINT(build/include_directory)
#include "v8-function.h"           // NOLINT(build/include_directory)
#include "v8-initialization.h"     // NOLINT(build/include_directory)
#include "v8-internal.h"           // NOLINT(build/include_directory)
#include "v8-isolate.h"            // NOLINT(build/include_directory)
#include "v8-json.h"               // NOLINT(build/include_directory)
#include "v8-local-handle.h"       // NOLINT(build/include_directory)
#include "v8-locker.h"             // NOLINT(build/include_directory)
#include "v8-maybe.h"              // NOLINT(build/include_directory)
#include "v8-memory-span.h"        // NOLINT(build/include_directory)
#include "v8-message.h"            // NOLINT(build/include_directory)
#include "v8-microtask-queue.h"    // NOLINT(build/include_directory)
#include "v8-microtask.h"          // NOLINT(build/include_directory)
#include "v8-object.h"             // NOLINT(build/include_directory)
#include "v8-persistent-handle.h"  // NOLINT(build/include_directory)
#include "v8-primitive-object.h"   // NOLINT(build/include_directory)
#include "v8-primitive.h"          // NOLINT(build/include_directory)
#include "v8-promise.h"            // NOLINT(build/include_directory)
#include "v8-proxy.h"              // NOLINT(build/include_directory)
#include "v8-regexp.h"             // NOLINT(build/include_directory)
#include "v8-script.h"             // NOLINT(build/include_directory)
#include "v8-snapshot.h"           // NOLINT(build/include_directory)
#include "v8-statistics.h"         // NOLINT(build/include_directory)
#include "v8-template.h"           // NOLINT(build/include_directory)
#include "v8-traced-handle.h"      // NOLINT(build/include_directory)
#include "v8-typed-array.h"        // NOLINT(build/include_directory)
#include "v8-unwinder.h"           // NOLINT(build/include_directory)
#include "v8-value-serializer.h"   // NOLINT(build/include_directory)
#include "v8-value.h"              // NOLINT(build/include_directory)
#include "v8-version.h"            // NOLINT(build/include_directory)
#include "v8-wasm.h"               // NOLINT(build/include_directory)
#include "v8config.h"              // NOLINT(build/include_directory)

#include "replayio-macros.h"

// We reserve the V8_* prefix for macros defined in V8 public API and
// assume there are no name conflicts with the embedder's code.

/**
 * The v8 JavaScript engine.
 */
namespace v8 {

class Platform;

/**
 * \example shell.cc
 * A simple shell that takes a list of expressions on the
 * command-line and executes them.
 */

/**
 * \example process.cc
 */

bool IsMainThread();

// Static container class for record/replay methods.
class V8_EXPORT recordreplay {
  public:

static void SetRecordingOrReplaying(void* handle);
// Whether this process is recording or replaying, and when a feature is
// given, whether the driver has left that feature enabled.
static bool IsRecordingOrReplaying(const char* feature = nullptr,
                                   const char* subfeature = nullptr);
static bool FeatureEnabled(const char* feature, const char* subfeature = nullptr);
static bool IsRecording();
static bool IsReplaying();

static void Print(const char* format, ...);
static void Diagnostic(const char* format, ...);
// Adds a warning about unexpected behavior to the recording. kind names the
// condition, fields hold the details that differ between occurrences.
static void Warning(const char* kind,
                    std::initializer_list<RecordReplayWarningField> fields = {});
static bool HasAsserts();
static void Assert(const char* format, ...);
static void AssertMaybeEventsDisallowed(const char* format, ...);
// Asserts the message both when constructed and when destroyed, unless events
// are disallowed, so the recording orders entering and leaving the scope.
struct AutoAssertMaybeEventsDisallowed {
  AutoAssertMaybeEventsDisallowed(const char* format, ...);
  ~AutoAssertMaybeEventsDisallowed();
  std::string msg_;
};
static void AssertBytes(const char* why, const void* buf, size_t size);
static void AssertScriptedCaller(Isolate* isolate, const char* why);
static bool AreAssertsDisabled();

static uintptr_t RecordReplayValue(const char* why, uintptr_t v);
static void RecordReplayBytes(const char* why, void* buf, size_t size);

static size_t CreateOrderedLock(const char* name);
static void OrderedLock(int lock);
static void OrderedUnlock(int lock);

struct AutoOrderedLock {
  AutoOrderedLock(int id) : id_(id) { OrderedLock(id_); }
  ~AutoOrderedLock() { OrderedUnlock(id_); }
  int id_;
};

static void InvalidateRecording(const char* why);
static void NewCheckpoint();

static void BeginPassThroughEvents();
static void EndPassThroughEvents();
static bool AreEventsPassedThrough(const char* why = nullptr);

static void BeginDisallowEvents();
static void BeginDisallowEventsWithLabel(const char* label);
static void EndDisallowEvents();

// A "why" string should be used whenever there are substantive behavior changes
// resulting from this check.
static bool AreEventsDisallowed(const char* why = nullptr);

// A "why" string should be used whenever there are substantive behavior changes
// resulting from this check.
static bool IsInReplayCode(const char* why = nullptr);

static void EnterReplayCode();
static void ExitReplayCode();

static void RegisterPointer(const void* ptr);
static void UnregisterPointer(const void* ptr);
static int PointerId(const void* ptr);
static void* IdPointer(int id);

static bool HasDivergedFromRecording();
static bool AllowSideEffects();
static void OnAnnotation(const char* kind, const char* contents);

// Long running work that doesn't advance the progress counter calls this
// regularly, so replay hang detection sees the process is busy.
static void NotifyActivity();

}; // class recordreplay

}  // namespace v8

#endif  // INCLUDE_V8_H_
