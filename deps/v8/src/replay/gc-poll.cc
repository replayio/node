#include "src/replay/gc-poll.h"

#include <vector>

#include "include/replayio.h"
#include "src/execution/isolate.h"
#include "src/replay/finalization-registry.h"
#include "src/replay/replay-isolate-data.h"
#include "src/replay/replayio.h"
#include "src/replay/weak-refs.h"

namespace v8 {
namespace replayio {

namespace i = internal;

namespace {

constexpr uintptr_t kPostCleanupTask = 1 << 0;
constexpr uintptr_t kCollectedRegistries = 1 << 1;
constexpr uintptr_t kClearedWeakRefs = 1 << 2;

void RecordReplayIds(const char* why, std::vector<int>* ids) {
  ids->resize(recordreplay::RecordReplayValue(why, ids->size()));
  recordreplay::RecordReplayBytes(why, ids->data(), ids->size() * sizeof(int));
}

}  // namespace

void ReplayGCPoll::Poll(i::Isolate* isolate) {
  ReplayIsolateData* data = isolate->replay_data();
  if (!data ||
      !(data->has_registered_weak_cells() || data->has_tracked_weak_refs())) {
    return;
  }
  if (!AreEventsAvailable()) return;

  uintptr_t flags = 0;
  std::vector<int> collected_registries;
  std::vector<int> cleared_weak_refs;
  if (recordreplay::IsRecording()) {
    if (ReplayFinalizationRegistries::ShouldPostCleanupTask(isolate)) {
      flags |= kPostCleanupTask;
    }
    ReplayFinalizationRegistries::TakeCollected(isolate, &collected_registries);
    if (!collected_registries.empty()) flags |= kCollectedRegistries;
    ReplayWeakRefs::TakeCleared(isolate, &cleared_weak_refs);
    if (!cleared_weak_refs.empty()) flags |= kClearedWeakRefs;
  }
  flags = recordreplay::RecordReplayValue("GC.poll", flags);

  if (flags & kCollectedRegistries) {
    RecordReplayIds("GC.poll collected registries", &collected_registries);
    if (recordreplay::IsReplaying()) {
      ReplayFinalizationRegistries::ReleaseCollected(isolate,
                                                     collected_registries);
    }
  }
  if (flags & kClearedWeakRefs) {
    RecordReplayIds("GC.poll cleared WeakRefs", &cleared_weak_refs);
    if (recordreplay::IsReplaying()) {
      ReplayWeakRefs::ClearTargets(isolate, cleared_weak_refs);
    }
  }
  if (flags & kPostCleanupTask) {
    ReplayFinalizationRegistries::PostCleanupTask(isolate);
  }
}

}  // namespace replayio
}  // namespace v8
