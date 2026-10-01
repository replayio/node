#ifndef V8_REPLAY_FINALIZATION_REGISTRY_H_
#define V8_REPLAY_FINALIZATION_REGISTRY_H_

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
//   cleanup task. Poll() schedules it from a point that replays, and the
//   cleanup loop records the id of each cell right before its callback runs.
// - When replaying, the GC treats the targets of cells as strong, so it never
//   clears one. Poll() schedules the task where the recording did, and the
//   cleanup loop clears the cells named by the recording before running their
//   callbacks. Tracked registries with registered cells are retained until
//   the recording shows that its GC collected them.
//
// Only registries constructed at a point which replays are handled this way
// ("tracked", JSFinalizationRegistry::replay_id != 0). Other registries get
// the default handling when both recording and replaying.
class ReplayFinalizationRegistries {
 public:
  // WeakCell::replay_id of a cell in a tracked registry which was registered
  // after diverging from the recording. It is retained but never delivered.
  static constexpr int kUndeliverableCellId = -1;

  // Whether the "finalization-registry" feature is active. When it is not,
  // FinalizationRegistry behaves as it does without this class.
  static bool Enabled();

  static void OnConstruct(
      internal::Isolate* isolate,
      internal::Handle<internal::JSFinalizationRegistry> registry);
  static void OnRegister(
      internal::Isolate* isolate,
      internal::Handle<internal::JSFinalizationRegistry> registry,
      internal::Handle<internal::WeakCell> cell);

  // Called when unregister() removes |cell| from |registry|. Cannot GC.
  static void OnUnregisterCell(internal::Isolate* isolate,
                               internal::JSFinalizationRegistry registry,
                               internal::WeakCell cell);

  // Called by the cleanup loop before it pops a cleared cell. Returns whether
  // the loop should continue. For a tracked registry this records/replays
  // which cell is delivered next.
  static bool NextCell(
      internal::Isolate* isolate,
      internal::Handle<internal::JSFinalizationRegistry> registry);

  // Schedules the cleanup task for tracked registries where the recording
  // did. Called at the end of every microtask checkpoint.
  static void Poll(internal::Isolate* isolate);

  // Picks the tracked registry the cleanup task posted by Poll() runs for.
  static internal::MaybeHandle<internal::JSFinalizationRegistry>
  TakeRegistryForTask(internal::Isolate* isolate);
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_FINALIZATION_REGISTRY_H_
