#include "src/replay/finalization-registry.h"

#include "include/replayio.h"
#include "include/v8-platform.h"
#include "src/api/api-inl.h"
#include "src/execution/isolate.h"
#include "src/heap/finalization-registry-cleanup-task.h"
#include "src/heap/heap-inl.h"
#include "src/init/v8.h"
#include "src/objects/dictionary-inl.h"
#include "src/objects/js-weak-refs-inl.h"
#include "src/replay/replay-isolate-data.h"

namespace v8 {
namespace replayio {

namespace i = internal;

namespace {

// Whether the current point replays, so that values can be recorded/replayed
// and ids handed out consistently.
bool EventsAvailable() {
  return !recordreplay::AreEventsDisallowed() &&
         !recordreplay::AreEventsPassedThrough() &&
         !recordreplay::HasDivergedFromRecording();
}

i::Handle<i::JSFinalizationRegistry> LookupRegistry(i::Isolate* isolate,
                                                    int id) {
  auto& registries = isolate->EnsureReplayData()->finalization_registries();
  auto it = registries.find(id);
  CHECK_WITH_MSG(it != registries.end(),
                 "Recorded FinalizationRegistry cleanup for unknown registry");
  v8::Local<v8::Value> local =
      it->second.Get(reinterpret_cast<v8::Isolate*>(isolate));
  return i::Handle<i::JSFinalizationRegistry>::cast(Utils::OpenHandle(*local));
}

// Runs inside the GC, so this only notes the id for the next Poll().
void OnRecordedRegistryCollected(
    const v8::WeakCallbackInfo<RecordedFinalizationRegistry>& info) {
  RecordedFinalizationRegistry* recorded = info.GetParameter();
  recorded->registry.Reset();
  recorded->data->collected_finalization_registries().push_back(recorded->id);
}

}  // namespace

bool ReplayFinalizationRegistries::Enabled() {
  return recordreplay::IsRecordingOrReplaying("finalization-registry");
}

void ReplayFinalizationRegistries::OnConstruct(
    i::Isolate* isolate, i::Handle<i::JSFinalizationRegistry> registry) {
  if (!Enabled() || !EventsAvailable()) return;

  int id = isolate->EnsureReplayData()->NewFinalizationRegistryId();
  recordreplay::Assert("FinalizationRegistry.construct %d", id);
  registry->set_replay_id(id);
}

void ReplayFinalizationRegistries::OnRegister(
    i::Isolate* isolate, i::Handle<i::JSFinalizationRegistry> registry,
    i::Handle<i::WeakCell> cell) {
  if (!registry->replay_id()) return;

  if (recordreplay::HasDivergedFromRecording()) {
    cell->set_replay_id(kUndeliverableCellId);
    return;
  }

  // The recording cannot describe when this cell is cleared, and the registry's
  // other cells are delivered from the recording.
  CHECK_WITH_MSG(
      EventsAvailable(),
      "FinalizationRegistry.prototype.register on a replay-tracked registry "
      "while events are disallowed");

  ReplayIsolateData* data = isolate->EnsureReplayData();
  int id = data->NewWeakCellId();
  recordreplay::Assert("FinalizationRegistry.register %d %d",
                       registry->replay_id(), id);
  cell->set_replay_id(id);
  data->set_has_registered_weak_cells();

  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8::Local<v8::Value> local = Utils::ToLocal(i::Handle<i::JSObject>(registry));

  if (!recordreplay::IsReplaying()) {
    auto& recorded = data->recorded_finalization_registries();
    if (!recorded.count(registry->replay_id())) {
      auto entry = std::make_unique<RecordedFinalizationRegistry>();
      entry->data = data;
      entry->id = registry->replay_id();
      entry->registry.Reset(v8_isolate, local);
      entry->registry.SetWeak(entry.get(), OnRecordedRegistryCollected,
                              v8::WeakCallbackType::kParameter);
      recorded.emplace(entry->id, std::move(entry));
    }
    return;
  }

  i::Handle<i::SimpleNumberDictionary> cells;
  if (registry->replay_cells().IsUndefined(isolate)) {
    cells = i::SimpleNumberDictionary::New(isolate, 1);
    // The recording can deliver callbacks of a registry which is no longer
    // reachable, as long as the recording's GC did not collect it.
    data->finalization_registries().emplace(
        registry->replay_id(), v8::Global<v8::Value>(v8_isolate, local));
  } else {
    cells = i::handle(i::SimpleNumberDictionary::cast(registry->replay_cells()),
                      isolate);
  }
  cells = i::SimpleNumberDictionary::Set(isolate, cells, id, cell);
  registry->set_replay_cells(*cells);
}

void ReplayFinalizationRegistries::OnUnregisterCell(
    i::Isolate* isolate, i::JSFinalizationRegistry registry, i::WeakCell cell) {
  if (!cell.replay_id() || recordreplay::HasDivergedFromRecording()) return;

  // The other side would still have this cell registered, and deliver it or
  // look for it according to the recording.
  CHECK_WITH_MSG(
      EventsAvailable(),
      "FinalizationRegistry.prototype.unregister on a replay-tracked registry "
      "while events are disallowed");

  if (registry.replay_cells().IsUndefined(isolate)) return;
  i::SimpleNumberDictionary cells =
      i::SimpleNumberDictionary::cast(registry.replay_cells());
  i::InternalIndex entry = cells.FindEntry(isolate, cell.replay_id());
  if (entry.is_found()) {
    cells.ClearEntry(entry);
    cells.ElementRemoved();
  }
}

bool ReplayFinalizationRegistries::NextCell(
    i::Isolate* isolate, i::Handle<i::JSFinalizationRegistry> registry) {
  if (!registry->replay_id()) return true;
  if (!EventsAvailable()) return false;

  uintptr_t id = 0;
  if (recordreplay::IsRecording() && registry->NeedsCleanup()) {
    int cell_id = i::WeakCell::cast(registry->cleared_cells()).replay_id();
    CHECK_GT(cell_id, 0);
    id = cell_id;
  }
  id = recordreplay::RecordReplayValue("FinalizationRegistry.cleanup", id);
  if (!id) return false;

  if (recordreplay::IsReplaying()) {
    CHECK_WITH_MSG(!registry->replay_cells().IsUndefined(isolate),
                   "Recorded FinalizationRegistry cleanup for unknown cell");
    i::Handle<i::SimpleNumberDictionary> cells = i::handle(
        i::SimpleNumberDictionary::cast(registry->replay_cells()), isolate);
    i::InternalIndex entry =
        cells->FindEntry(isolate, static_cast<uint32_t>(id));
    CHECK_WITH_MSG(entry.is_found(),
                   "Recorded FinalizationRegistry cleanup for unknown cell");
    i::WeakCell cell = i::WeakCell::cast(cells->ValueAt(entry));
    // Do what the recording's GC did when it found the target dead: this moves
    // the cell to the head of the cleared list, where the cleanup loop pops it.
    cell.Nullify(isolate, [](i::HeapObject, i::ObjectSlot, i::Object) {});
    cells = i::SimpleNumberDictionary::DeleteEntry(isolate, cells, entry);
    registry->set_replay_cells(*cells);
  }
  return true;
}

void ReplayFinalizationRegistries::Poll(i::Isolate* isolate) {
  ReplayIsolateData* data = isolate->replay_data();
  if (!data || !data->has_registered_weak_cells()) return;
  if (!Enabled() || !EventsAvailable()) return;

  i::Heap* heap = isolate->heap();
  uintptr_t post = recordreplay::IsRecording() &&
                   !data->finalization_registry_task_posted() &&
                   heap->RecordReplayHasDirtyJSFinalizationRegistries(true);

  // One value carries both whether to post the cleanup task and how many
  // registries the recording's GC collected since the last poll.
  std::vector<int> collected;
  if (recordreplay::IsRecording()) {
    collected.swap(data->collected_finalization_registries());
    for (int id : collected) data->recorded_finalization_registries().erase(id);
  }
  uintptr_t value = recordreplay::RecordReplayValue(
      "FinalizationRegistry.schedule", post | (collected.size() << 1));
  post = value & 1;
  if (size_t collected_count = value >> 1) {
    collected.resize(collected_count);
    recordreplay::RecordReplayBytes("FinalizationRegistry.collected",
                                    collected.data(),
                                    collected_count * sizeof(int));
    // The recording cannot deliver from these registries anymore.
    if (recordreplay::IsReplaying()) {
      for (int id : collected) data->finalization_registries().erase(id);
    }
  }
  if (!post) return;

  data->set_finalization_registry_task_posted(true);
  auto taskrunner = i::V8::GetCurrentPlatform()->GetForegroundTaskRunner(
      reinterpret_cast<v8::Isolate*>(isolate));
  taskrunner->PostNonNestableTask(
      std::make_unique<i::FinalizationRegistryCleanupTask>(
          heap, i::FinalizationRegistryCleanupTask::kReplayTracked));
}

i::MaybeHandle<i::JSFinalizationRegistry>
ReplayFinalizationRegistries::TakeRegistryForTask(i::Isolate* isolate) {
  isolate->EnsureReplayData()->set_finalization_registry_task_posted(false);
  if (!EventsAvailable()) return {};

  i::MaybeHandle<i::JSFinalizationRegistry> registry;
  uintptr_t id = 0;
  if (recordreplay::IsRecording()) {
    registry =
        isolate->heap()->RecordReplayDequeueDirtyJSFinalizationRegistry(true);
    if (!registry.is_null()) id = registry.ToHandleChecked()->replay_id();
  }
  id = recordreplay::RecordReplayValue("FinalizationRegistry.task", id);
  if (recordreplay::IsReplaying() && id) {
    registry = LookupRegistry(isolate, static_cast<int>(id));
  }
  return registry;
}

}  // namespace replayio
}  // namespace v8
