#ifndef V8_REPLAY_REPLAY_ISOLATE_DATA_H_
#define V8_REPLAY_REPLAY_ISOLATE_DATA_H_

#include <memory>
#include <unordered_map>
#include <vector>

#include "include/v8-internal.h"
#include "include/v8.h"

namespace v8 {
namespace replayio {

// General-purpose per-Isolate data for recording and replaying.
class ReplayIsolateData {
 public:
  ReplayIsolateData() = default;
  ~ReplayIsolateData();

  ReplayIsolateData(const ReplayIsolateData&) = delete;
  ReplayIsolateData& operator=(const ReplayIsolateData&) = delete;

  // A tracked FinalizationRegistry with registered cells, watched while
  // recording so that the replay can be told when the GC collected it.
  struct RecordedFinalizationRegistry {
    ReplayIsolateData* data;
    int id;
    v8::Global<v8::Value> registry;
  };

  // FinalizationRegistry state, see src/replay/finalization-registry.h.

  int NewFinalizationRegistryId() { return next_finalization_registry_id_++; }
  int NewWeakCellId() { return next_weak_cell_id_++; }

  bool has_registered_weak_cells() const { return has_registered_weak_cells_; }
  void set_has_registered_weak_cells() { has_registered_weak_cells_ = true; }

  bool is_finalization_registry_cleanup_task_posted() const {
    return is_finalization_registry_cleanup_task_posted_;
  }
  void set_is_finalization_registry_cleanup_task_posted(bool posted) {
    is_finalization_registry_cleanup_task_posted_ = posted;
  }

  // While replaying, the tracked registries with registered cells, by
  // JSFinalizationRegistry::record_replay_id. An entry is dropped once the
  // recording shows that the GC collected the registry.
  std::unordered_map<int, v8::Global<v8::Value>>&
  retained_finalization_registries() {
    return retained_finalization_registries_;
  }

  // While recording, weak handles to the same set of registries.
  std::unordered_map<int, std::unique_ptr<RecordedFinalizationRegistry>>&
  recorded_finalization_registries() {
    return recorded_finalization_registries_;
  }

  // While recording, ids of registries the GC collected which the recording
  // does not describe yet.
  std::vector<int>& collected_finalization_registries() {
    return collected_finalization_registries_;
  }

  // WeakRef state, see src/replay/weak-refs.h.

  int NewWeakRefId() {
    has_tracked_weak_refs_ = true;
    return next_weak_ref_id_++;
  }
  bool has_tracked_weak_refs() const { return has_tracked_weak_refs_; }

  // While recording, ids of tracked WeakRefs whose target the GC cleared and
  // which the recording does not describe yet.
  std::vector<int>& cleared_weak_refs() { return cleared_weak_refs_; }

  // While replaying, the location of a global handle to a WeakArrayList of the
  // tracked WeakRefs, or null before the first one.
  internal::Address* tracked_weak_refs_location() const {
    return tracked_weak_refs_location_;
  }
  void set_tracked_weak_refs_location(internal::Address* location) {
    tracked_weak_refs_location_ = location;
  }

 private:
  int next_finalization_registry_id_ = 1;
  int next_weak_cell_id_ = 1;

  // Set by the first FinalizationRegistry.prototype.register() call on a
  // tracked registry and never cleared. While unset, and without a tracked
  // WeakRef, the ClearKeptObjects poll records nothing.
  //
  // A count of outstanding registrations could stop the poll from recording
  // again once it drops to zero, as long as there is no tracked WeakRef
  // either, and shrink recordings. To be correct it would have to:
  //   - decrement on callback delivery and on unregister() (per removed cell),
  //   - account for the cells of a collected registry only once the recording
  //     describes the collection, which is when replay learns about it,
  //   - ignore cells registered after diverging.
  // A mismatch desyncs the recorded value stream. Typical users keep
  // registrations outstanding for the life of the page, so the count would
  // rarely return to zero and is unlikely to be worth it.
  bool has_registered_weak_cells_ = false;

  // Whether a cleanup task for tracked registries is posted and has not run.
  bool is_finalization_registry_cleanup_task_posted_ = false;

  std::unordered_map<int, v8::Global<v8::Value>>
      retained_finalization_registries_;
  std::unordered_map<int, std::unique_ptr<RecordedFinalizationRegistry>>
      recorded_finalization_registries_;
  std::vector<int> collected_finalization_registries_;

  int next_weak_ref_id_ = 1;
  // Set by the first tracked WeakRef and never cleared. From then on the
  // ClearKeptObjects poll records a value at every microtask checkpoint.
  bool has_tracked_weak_refs_ = false;
  std::vector<int> cleared_weak_refs_;
  internal::Address* tracked_weak_refs_location_ = nullptr;
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_REPLAY_ISOLATE_DATA_H_
