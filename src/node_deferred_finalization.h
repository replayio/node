#ifndef SRC_NODE_DEFERRED_FINALIZATION_H_
#define SRC_NODE_DEFERRED_FINALIZATION_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <unordered_map>
#include <vector>

#include "v8.h"

namespace node {

class BaseObject;
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
// Two kinds of notes:
// - The async ids of resources the GC destroyed, see AsyncWrap::EmitDestroy.
//   A destroy is an event with an integer id, so nothing has to be kept alive
//   for the replay to deliver it.
// - BaseObjects with a record/replay id, see BaseObject::RecordReplayTrack.
//   When recording, the weak callback leaves the C++ object alive until the
//   poll runs its OnGCCollect(). When replaying, the JS object is kept alive
//   too (MakeWeak() has no effect) until the poll names the id, so that the
//   object is in the same state on both sides when its cleanup runs.
//
// Only objects created at a point which replays (AreEventsRecorded()) get an
// id. For the others, the weak callback runs as usual and subclasses fall back
// to leaking (see EnterLeakMemory). Switched off by the "deferred-finalization"
// feature, which leaves the cleanup dropped or leaked as before.
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

  // Called by BaseObject::RecordReplayTrack. Gives object an id when it is
  // created at a point which replays, or returns 0.
  int Track(BaseObject* object, const char* label);
  // Called by ~BaseObject for a tracked object.
  void Untrack(int id);
  // Called by the weak callback of a tracked object, which only runs when
  // recording: notes the id for the next poll.
  void OnCollected(int id);

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

  int last_object_id_ = 0;
  // Set by the first tracked object and never cleared. From then on the poll
  // records a value at every microtask checkpoint.
  bool has_tracked_objects_ = false;
  // The tracked objects which are alive, by id. The same on both sides at
  // every poll: a tracked object is only deleted at points which replay.
  std::unordered_map<int, BaseObject*> tracked_objects_;
  // While recording, the ids of tracked objects the GC collected which the
  // recording does not describe yet.
  std::vector<int> collected_objects_;
};

}  // namespace recordreplay

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_DEFERRED_FINALIZATION_H_
