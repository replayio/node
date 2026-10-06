#ifndef SRC_NODE_DEFERRED_FINALIZATION_H_
#define SRC_NODE_DEFERRED_FINALIZATION_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <unordered_map>
#include <vector>

#include "node_mutex.h"
#include "v8.h"

namespace node {

class Environment;

namespace recordreplay {

// Something DeferredFinalization runs the cleanup of, see below.
class Finalizable {
 public:
  // Called by DeferredFinalization::Poll, on both sides, where the recording's
  // GC was found to have collected this: does what the GC's callback would have
  // done when recording, and additionally releases whatever was kept alive when
  // replaying.
  virtual void RecordReplayFinalize() = 0;

 protected:
  virtual ~Finalizable() = default;
};

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
// - Finalizables with a record/replay id, given by Track(): BaseObjects (see
//   BaseObject::RecordReplayTrack), N-API references (v8impl::Reference) and
//   the free callbacks of external Buffers (Buffer::CallbackInfo). When
//   recording, the GC's callback leaves the C++ object alive until the poll
//   calls RecordReplayFinalize(). When replaying, what the GC would collect is
//   kept alive too (e.g. MakeWeak() has no effect) until the poll names the id,
//   so that the object is in the same state on both sides when its cleanup
//   runs.
//
// Only objects created at a point which replays (AreEventsRecorded()) get an
// id. For the others, the GC's callback runs as usual and the users fall back
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

  // Gives object an id when it is created at a point which replays, or
  // returns 0. The label is the subfeature which can switch this off.
  int Track(Finalizable* object, const char* label);
  // Called when a tracked object is deleted, from a point which replays.
  void Untrack(int id);
  // Called by the GC's callback for a tracked object, which only runs when
  // recording: notes the id for the next poll. Can be called from any thread,
  // e.g. a backing store's deleter runs on the thread which sweeps it.
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
  std::unordered_map<int, Finalizable*> tracked_objects_;
  // While recording, the ids of tracked objects the GC collected which the
  // recording does not describe yet.
  Mutex collected_objects_mutex_;
  std::vector<int> collected_objects_;
};

}  // namespace recordreplay

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_DEFERRED_FINALIZATION_H_
