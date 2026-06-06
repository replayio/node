// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INCLUDE_V8_H_
#define INCLUDE_V8_H_

/** \mainpage V8 API Reference Guide
 *
 * V8 is Google's open source JavaScript engine.
 *
 * This set of documents provides reference material generated from the
 * V8 header files in the include/ subdirectory.
 *
 * For other documentation see https://v8.dev/.
 */

#include <stddef.h>
#include <stdint.h>
#include <string>

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
static bool IsRecordingOrReplaying(const char* feature = nullptr,
                                   const char* subfeature = nullptr);
static bool IsRecording();
static bool IsReplaying();
static const char* GetRecordingId();

static bool IsARMRecording();

static bool FeatureEnabled(const char* feature, const char* subfeature = nullptr);
static bool HasDisabledFeatures();

static char* ReadAssetFileContents(const char* path, size_t* size);
static void Print(const char* format, ...);
static void Diagnostic(const char* format, ...);
static void CommandDiagnostic(const char* format, ...);
static void CommandDiagnosticTrace(const char* format, ...);
static void Warning(const char* format, ...);
static void Trace(const char* format, ...);
static void Crash(const char* format, ...);
static bool HadMismatch();
static bool HasAsserts();
static void AssertRaw(const char* format, ...);
static void Assert(const char* format, ...);
static void AssertMaybeEventsDisallowed(const char* format, ...);
static void AssertBytes(const char* why, const void* buf, size_t size);
static bool AreAssertsDisabled();

static uintptr_t RecordReplayValue(const char* why, uintptr_t v);
static void RecordReplayBytes(const char* why, void* buf, size_t size);
static void RecordReplayString(const char* why, std::string& str);

static size_t CreateOrderedLock(const char* name);
static void OrderedLock(int lock);
static void OrderedUnlock(int lock);

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

static bool HasDivergedFromRecording();
static bool AllowSideEffects();

static int NewDependencyGraphNode(const char* json);
static void AddDependencyGraphEdge(int source, int target, const char* json);
static void BeginDependencyExecution(int node);
static void EndDependencyExecution();

struct AutoDependencyExecution {
  AutoDependencyExecution(int node) {
    BeginDependencyExecution(node);
  }
  ~AutoDependencyExecution() {
    EndDependencyExecution();
  }
};

static void BeginAssertBufferAllocations(const char* issueLabel = "");
static void EndAssertBufferAllocations();

struct AutoOrderedLock {
  AutoOrderedLock(int id) : id_(id) { OrderedLock(id_); }
  ~AutoOrderedLock() { OrderedUnlock(id_); }
  int id_;
};

struct AutoAssertMaybeEventsDisallowed {
  AutoAssertMaybeEventsDisallowed(const char* format, ...);
  ~AutoAssertMaybeEventsDisallowed();
  std::string msg_;
};

struct AssertBufferAllocationState {
  size_t enabled = 0;
  std::string issueLabel = "";
};


// RAII class to enable recording assertions on dynamic-length buffer 
// allocations. Used to track down the allocation causing mismatched message 
// sizes when replaying.
// TODO: Merge this with the similar `AuxtoRecordReplayAssertBufferAllocations`
// in mojo/public/cpp/bindings/lib/buffer.cc (for main-thread only).
struct AutoAssertBufferAllocations {
  static AssertBufferAllocationState* GetState();

  AutoAssertBufferAllocations(const char* issueLabel = "");
  ~AutoAssertBufferAllocations();
};

static void RegisterPointer(const char* name, const void* ptr);
static void UnregisterPointer(const void* ptr);
static int PointerId(const void* ptr);
static void* IdPointer(int id);

}; // class recordreplay

}  // namespace v8

#endif  // INCLUDE_V8_H_
