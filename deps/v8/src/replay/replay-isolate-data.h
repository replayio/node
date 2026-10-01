#ifndef V8_REPLAY_REPLAY_ISOLATE_DATA_H_
#define V8_REPLAY_REPLAY_ISOLATE_DATA_H_

#include <memory>
#include <unordered_map>
#include <vector>

#include "include/v8.h"

namespace v8 {
namespace replayio {

class ReplayIsolateData;

// A tracked FinalizationRegistry with registered cells, watched while recording
// so that the replay can be told when the GC collected it.
struct RecordedFinalizationRegistry {
  ReplayIsolateData* data;
  int id;
  v8::Global<v8::Value> registry;
};

// General-purpose per-Isolate data for recording and replaying.
class ReplayIsolateData {
 public:
  ReplayIsolateData() = default;
  ~ReplayIsolateData() = default;

  ReplayIsolateData(const ReplayIsolateData&) = delete;
  ReplayIsolateData& operator=(const ReplayIsolateData&) = delete;

  int NewFinalizationRegistryId() { return next_finalization_registry_id_++; }
  int NewWeakCellId() { return next_weak_cell_id_++; }

  bool has_registered_weak_cells() const { return has_registered_weak_cells_; }
  void set_has_registered_weak_cells() { has_registered_weak_cells_ = true; }

  bool finalization_registry_task_posted() const {
    return finalization_registry_task_posted_;
  }
  void set_finalization_registry_task_posted(bool posted) {
    finalization_registry_task_posted_ = posted;
  }

  // While replaying, the tracked registries with registered cells, by
  // JSFinalizationRegistry::replay_id. An entry is dropped once the recording
  // shows that the GC collected the registry.
  std::unordered_map<int, v8::Global<v8::Value>>& finalization_registries() {
    return finalization_registries_;
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

 private:
  int next_finalization_registry_id_ = 1;
  int next_weak_cell_id_ = 1;

  // Set by the first FinalizationRegistry.prototype.register() call on a
  // tracked registry and never cleared. While unset, the ClearKeptObjects poll
  // records nothing.
  //
  // A count of outstanding registrations could stop the poll from recording
  // again once it drops to zero, and shrink recordings. To be correct it would
  // have to:
  //   - decrement on callback delivery and on unregister() (per removed cell),
  //   - never decrement when the GC collects a registry, since replay cannot
  //     observe that,
  //   - ignore cells registered while events are disallowed or after diverging.
  // A mismatch desyncs the recorded value stream. Typical users keep
  // registrations outstanding for the life of the page, so the count would
  // rarely return to zero and is unlikely to be worth it.
  bool has_registered_weak_cells_ = false;

  // Whether a cleanup task for tracked registries is posted and has not run.
  bool finalization_registry_task_posted_ = false;

  std::unordered_map<int, v8::Global<v8::Value>> finalization_registries_;
  std::unordered_map<int, std::unique_ptr<RecordedFinalizationRegistry>>
      recorded_finalization_registries_;
  std::vector<int> collected_finalization_registries_;
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_REPLAY_ISOLATE_DATA_H_
