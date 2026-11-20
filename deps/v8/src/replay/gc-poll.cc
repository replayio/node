#include "src/replay/gc-poll.h"

#include <algorithm>
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
constexpr uintptr_t kEmbedderPending = 1 << 3;

void RecordReplayIds(const char* why, std::vector<int>* ids) {
  ids->resize(recordreplay::RecordReplayValue(why, ids->size()));
  recordreplay::RecordReplayBytes(why, ids->data(), ids->size() * sizeof(int));
}

}  // namespace

void ReplayGCPoll::Poll(i::Isolate* isolate) {
  ReplayIsolateData* data = isolate->replay_data();
  // A participant's cleanup can run embedder code which reaches another poll.
  // That poll does nothing, on both sides. So does one the embedder suspended,
  // see AutoSuspendReplayGCPoll.
  if (!data || data->is_gc_polling() || data->gc_poll_suspended_depth()) return;
  bool v8_active =
      data->has_registered_weak_cells() || data->has_tracked_weak_refs();
  std::vector<ReplayGCPollParticipant*>& participants =
      data->active_gc_poll_participants();
  participants.clear();
  for (ReplayGCPollParticipant* participant : data->gc_poll_participants()) {
    if (participant->IsActive()) participants.push_back(participant);
  }
  if (!v8_active && participants.empty()) return;
  if (!AreEventsAvailable()) return;
  data->set_is_gc_polling(true);

  uintptr_t flags = 0;
  std::vector<int> collected_registries;
  std::vector<int> cleared_weak_refs;
  if (recordreplay::IsRecording()) {
    if (v8_active) {
      if (ReplayFinalizationRegistries::ShouldPostCleanupTask(isolate)) {
        flags |= kPostCleanupTask;
      }
      ReplayFinalizationRegistries::TakeCollected(isolate,
                                                  &collected_registries);
      if (!collected_registries.empty()) flags |= kCollectedRegistries;
      ReplayWeakRefs::TakeCleared(isolate, &cleared_weak_refs);
      if (!cleared_weak_refs.empty()) flags |= kClearedWeakRefs;
    }
    for (ReplayGCPollParticipant* participant : participants) {
      if (participant->HasPending()) {
        flags |= kEmbedderPending;
        break;
      }
    }
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
  if (flags & kEmbedderPending) {
    CHECK_WITH_MSG(!participants.empty(),
                   "Recorded GC poll for embedder objects without an active "
                   "participant");
    for (ReplayGCPollParticipant* participant : participants) {
      participant->Poll();
    }
  }
  data->set_is_gc_polling(false);
}

AutoSuspendReplayGCPoll::AutoSuspendReplayGCPoll(Isolate* isolate)
    : isolate_(isolate), suspended_(false) {
  if (!recordreplay::IsRecordingOrReplaying()) return;
  // Without the data there is no poll to suspend: Poll() returns before it
  // records anything. The embedder has it from the point it added its first
  // participant, which node does for every isolate it runs an Environment on,
  // and this is not the place to make it: the module code which runs in these
  // scopes includes the finalizers the heap's tear down calls, after the data
  // was destroyed along with the Globals it holds.
  ReplayIsolateData* data =
      reinterpret_cast<i::Isolate*>(isolate_)->replay_data();
  if (!data) return;
  suspended_ = true;
  data->set_gc_poll_suspended_depth(data->gc_poll_suspended_depth() + 1);
}

AutoSuspendReplayGCPoll::~AutoSuspendReplayGCPoll() {
  if (!suspended_) return;
  ReplayIsolateData* data =
      reinterpret_cast<i::Isolate*>(isolate_)->replay_data();
  // Gone once the heap's tear down has started, as for a participant.
  if (!data) return;
  data->set_gc_poll_suspended_depth(data->gc_poll_suspended_depth() - 1);
}

void AddReplayGCPollParticipant(Isolate* isolate,
                                ReplayGCPollParticipant* participant) {
  ReplayIsolateData* data =
      reinterpret_cast<i::Isolate*>(isolate)->EnsureReplayData();
  CHECK(!data->is_gc_polling());
  data->gc_poll_participants().push_back(participant);
}

void RemoveReplayGCPollParticipant(Isolate* isolate,
                                   ReplayGCPollParticipant* participant) {
  ReplayIsolateData* data =
      reinterpret_cast<i::Isolate*>(isolate)->replay_data();
  // Gone once the heap's tear down has started.
  if (!data) return;
  CHECK(!data->is_gc_polling());
  std::vector<ReplayGCPollParticipant*>& participants =
      data->gc_poll_participants();
  participants.erase(
      std::remove(participants.begin(), participants.end(), participant),
      participants.end());
}

}  // namespace replayio
}  // namespace v8
