#ifndef V8_REPLAY_FINALIZATION_REGISTRY_H_
#define V8_REPLAY_FINALIZATION_REGISTRY_H_

#include <vector>

#include "src/handles/handles.h"
#include "src/handles/maybe-handles.h"

namespace v8 {
namespace internal {
class Isolate;
class JSFinalizationRegistry;
class WeakCell;
}  // namespace internal

namespace replayio {

// Deterministic FinalizationRegistry cleanup when recording/replaying.
//
// Which WeakCells the GC clears, and when, differs between recording and
// replaying, so the recording is the source of truth:
//
// - When recording, the GC clears cells as usual but does not schedule the
//   cleanup task. ReplayGCPoll::Poll schedules it from a point that replays,
//   and the cleanup loop records the id of each cell right before its callback
//   runs.
// - When replaying, the GC treats the targets of tracked cells as strong, so it
//   never clears one. ReplayGCPoll::Poll schedules the task where the recording
//   did, and the cleanup loop clears the cells named by the recording before
//   running their callbacks. Tracked registries with registered cells are
//   retained until the recording shows that its GC collected them.
//
// Only registries constructed at a point which replays are handled this way
// ("tracked", JSFinalizationRegistry::record_replay_id != 0), and none is
// unless the "finalization-registry" feature is active. Other registries get
// the default handling when both recording and replaying.
class ReplayFinalizationRegistries {
 public:
  // WeakCell::record_replay_id of a cell in a tracked registry which was
  // registered after diverging from the recording. It is retained but never
  // delivered.
  static constexpr int kUndeliverableCellId = -1;

  static void OnConstruct(
      internal::Isolate* isolate,
      internal::Handle<internal::JSFinalizationRegistry> registry);
  // Gives a registry which has no record/replay id and no cells one, so that
  // registries constructed where no id could be assigned (e.g. deserialized
  // from a snapshot) are tracked from their first use. Returns whether the
  // registry has an id afterwards.
  static bool Adopt(internal::Isolate* isolate,
                    internal::Handle<internal::JSFinalizationRegistry> registry);
  // Crashes for a tracked registry when the current point does not replay and
  // the process has not diverged, as the recording could not describe when the
  // new cell is cleared.
  static void OnRegister(
      internal::Isolate* isolate,
      internal::Handle<internal::JSFinalizationRegistry> registry,
      internal::Handle<internal::WeakCell> cell);

  // Called when unregister() removes |cell| from |registry|. Cannot GC. Crashes
  // for a tracked cell when the current point does not replay and the process
  // has not diverged.
  static void OnUnregisterCell(internal::Isolate* isolate,
                               internal::JSFinalizationRegistry registry,
                               internal::WeakCell cell);

  // Called by the cleanup loop before it pops a cleared cell. Returns whether
  // the loop should continue. For a tracked registry this records/replays
  // which cell is delivered next.
  static bool NextCell(
      internal::Isolate* isolate,
      internal::Handle<internal::JSFinalizationRegistry> registry);

  // The pieces ReplayGCPoll::Poll records/replays. When recording: whether a
  // cleanup task for tracked registries has to be posted, and the ids of the
  // registries the GC collected since the last poll.
  static bool ShouldPostCleanupTask(internal::Isolate* isolate);
  static void TakeCollected(internal::Isolate* isolate, std::vector<int>* ids);
  // When replaying: stop retaining the registries with these ids.
  static void ReleaseCollected(internal::Isolate* isolate,
                               const std::vector<int>& ids);
  // On both sides: post the cleanup task for tracked registries.
  static void PostCleanupTask(internal::Isolate* isolate);

  // Picks the tracked registry the cleanup task posted by PostCleanupTask()
  // runs for.
  static internal::MaybeHandle<internal::JSFinalizationRegistry>
  TakeRegistryForTask(internal::Isolate* isolate);
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_FINALIZATION_REGISTRY_H_
