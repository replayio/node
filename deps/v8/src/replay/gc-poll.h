#ifndef V8_REPLAY_GC_POLL_H_
#define V8_REPLAY_GC_POLL_H_

namespace v8 {
namespace internal {
class Isolate;
}  // namespace internal

namespace replayio {

// The GC runs at points which do not replay, so what it did to tracked
// FinalizationRegistries and WeakRefs is recorded/replayed here instead, with
// a single value when there is nothing to describe. See
// src/replay/finalization-registry.h and src/replay/weak-refs.h.
class ReplayGCPoll {
 public:
  // Called at the end of every microtask checkpoint, and when the cleanup task
  // for tracked registries is done.
  static void Poll(internal::Isolate* isolate);
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_GC_POLL_H_
