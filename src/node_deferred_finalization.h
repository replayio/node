#ifndef SRC_NODE_DEFERRED_FINALIZATION_H_
#define SRC_NODE_DEFERRED_FINALIZATION_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <vector>

#include "v8.h"

namespace node {

class Environment;

namespace recordreplay {

// Runs the cleanup for what the GC collects at a point which replays.
//
// When recording/replaying, old space GCs run at points the replay does not
// reproduce, and which embedder objects die in one differs between the two.
// Cleanup a weak callback runs is therefore either dropped or leaked until
// teardown, since the replay could not run it at the same point. Instead, like
// V8's WeakRef and FinalizationRegistry handling (deps/v8/src/replay/gc-poll.h),
// the weak callback only notes what the GC collected, and Poll(), run at every
// microtask checkpoint, writes the notes into the recording and runs the
// cleanup from there. When replaying, the recorded notes say what to clean up
// at the same checkpoint.
//
// The notes are the async ids of resources the GC destroyed, see
// AsyncWrap::EmitDestroy. A destroy is an event with an integer id, so nothing
// has to be kept alive for the replay to deliver it.
//
// Switched off by the "deferred-finalization" feature, which leaves the
// cleanup dropped or leaked as before.
class DeferredFinalization {
 public:
  explicit DeferredFinalization(Environment* env);
  ~DeferredFinalization();

  DeferredFinalization(const DeferredFinalization&) = delete;
  DeferredFinalization& operator=(const DeferredFinalization&) = delete;

  // Whether the mechanism handles what is labeled, when recording/replaying.
  static bool Enabled(const char* label);

  // Called by AsyncWrap::EmitDestroy for a destroy which comes from the GC.
  // Returns whether the destroy will be delivered from Poll(): when recording,
  // the id is noted; when replaying, the recording names the ids to deliver and
  // the replay's own GC destroys are dropped.
  bool AddDestroyedAsyncId(double async_id);

  // Records/replays what the GC collected since the last poll and runs the
  // cleanup. Called at the end of every microtask checkpoint.
  void Poll();

 private:
  static void MicrotasksCompleted(v8::Isolate* isolate, void* data);

  // Whether Poll() has anything to describe at this point, on both sides.
  bool ShouldPoll() const;

  Environment* env_;
  bool polling_ = false;

  // While recording, the async ids of resources the GC destroyed which the
  // recording does not describe yet.
  std::vector<double> destroyed_async_ids_;
};

}  // namespace recordreplay

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_DEFERRED_FINALIZATION_H_
