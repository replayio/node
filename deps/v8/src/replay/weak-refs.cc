#include "src/replay/weak-refs.h"

#include <vector>

#include "include/replayio.h"
#include "src/execution/isolate.h"
#include "src/handles/global-handles.h"
#include "src/heap/factory.h"
#include "src/objects/fixed-array-inl.h"
#include "src/objects/js-weak-refs-inl.h"
#include "src/replay/replay-isolate-data.h"
#include "src/replay/replayio.h"

namespace v8 {
namespace replayio {

namespace i = internal;

namespace {

bool Enabled() {
  return recordreplay::IsRecordingOrReplaying("weak-ref-collection");
}

}  // namespace

void ReplayWeakRefs::OnConstruct(i::Isolate* isolate,
                                 i::Handle<i::JSWeakRef> weak_ref) {
  if (!Enabled() || !AreEventsAvailable()) return;

  ReplayIsolateData* data = isolate->EnsureReplayData();
  int id = data->NewWeakRefId();
  recordreplay::Assert("WeakRef.construct %d", id);
  weak_ref->set_record_replay_id(id);

  if (!recordreplay::IsReplaying()) return;

  // The ids are consecutive, so the WeakRef with a given id is at index
  // id - 1. The list holds them weakly. It is allocated in old space, as a
  // global handle whose object is replaced is not tracked for scavenges.
  i::Handle<i::WeakArrayList> weak_refs;
  i::Address* location = data->tracked_weak_refs_location();
  if (location) {
    weak_refs = i::Handle<i::WeakArrayList>(location);
  } else {
    weak_refs = isolate->factory()->empty_weak_array_list();
  }
  CHECK_EQ(weak_refs->length(), id - 1);
  weak_refs = i::WeakArrayList::EnsureSpace(isolate, weak_refs, id,
                                            i::AllocationType::kOld);
  weak_refs->Set(id - 1, i::HeapObjectReference::Weak(*weak_ref));
  weak_refs->set_length(id);
  if (location) {
    *location = weak_refs->ptr();
  } else {
    data->set_tracked_weak_refs_location(
        isolate->global_handles()->Create(*weak_refs).location());
  }
}

void ReplayWeakRefs::OnTargetCleared(i::Isolate* isolate,
                                     i::JSWeakRef weak_ref) {
  // Runs inside the GC, so this only notes the id for the next poll.
  int id = weak_ref.record_replay_id();
  if (!id) return;
  ReplayIsolateData* data = isolate->replay_data();
  if (!data) return;
  data->cleared_weak_refs().push_back(id);
}

void ReplayWeakRefs::TakeCleared(i::Isolate* isolate, std::vector<int>* ids) {
  ids->swap(isolate->replay_data()->cleared_weak_refs());
}

void ReplayWeakRefs::ClearTargets(i::Isolate* isolate,
                                  const std::vector<int>& ids) {
  i::WeakArrayList weak_refs = i::WeakArrayList::cast(
      i::Object(*isolate->replay_data()->tracked_weak_refs_location()));
  for (int id : ids) {
    i::HeapObject weak_ref;
    // The replay's GC may have collected the WeakRef itself already.
    if (weak_refs.Get(id - 1)->GetHeapObjectIfWeak(&weak_ref)) {
      i::JSWeakRef::cast(weak_ref).set_target(
          i::ReadOnlyRoots(isolate).undefined_value());
    }
  }
}

}  // namespace replayio
}  // namespace v8
