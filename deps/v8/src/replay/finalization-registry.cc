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
#include "src/replay/replayio.h"

namespace v8 {
namespace replayio {

namespace i = internal;

namespace {

i::Handle<i::JSFinalizationRegistry> LookupRegistry(i::Isolate* isolate,
                                                    int id) {
  auto& registries = isolate->replay_data()->retained_finalization_registries();
  auto it = registries.find(id);
  CHECK_WITH_MSG(it != registries.end(),
                 "Recorded FinalizationRegistry cleanup for unknown registry");
  v8::Local<v8::Value> local =
      it->second.Get(reinterpret_cast<v8::Isolate*>(isolate));
  return i::Handle<i::JSFinalizationRegistry>::cast(Utils::OpenHandle(*local));
}

// Runs inside the GC, so this only notes the id for the next poll.
void OnRecordedRegistryCollected(
    const v8::WeakCallbackInfo<ReplayIsolateData::RecordedFinalizationRegistry>&
        info) {
  ReplayIsolateData::RecordedFinalizationRegistry* recorded =
      info.GetParameter();
  recorded->registry.Reset();
  recorded->data->collected_finalization_registries().push_back(recorded->id);
}

bool Enabled() {
  return recordreplay::IsRecordingOrReplaying("finalization-registry");
}

}  // namespace

void ReplayFinalizationRegistries::OnConstruct(
    i::Isolate* isolate, i::Handle<i::JSFinalizationRegistry> registry) {
  if (!Enabled() || !AreEventsAvailable()) return;

  int id = isolate->EnsureReplayData()->NewFinalizationRegistryId();
  recordreplay::Assert("FinalizationRegistry.construct %d", id);
  registry->set_record_replay_id(id);
}

bool ReplayFinalizationRegistries::Adopt(
    i::Isolate* isolate, i::Handle<i::JSFinalizationRegistry> registry) {
  if (registry->record_replay_id()) return true;
  if (!Enabled() || !AreEventsAvailable()) return false;
  // Cells registered before now have no ids, so the recording could not
  // describe when they are cleared.
  if (!registry->active_cells().IsUndefined(isolate) ||
      !registry->cleared_cells().IsUndefined(isolate)) {
    return false;
  }

  int id = isolate->EnsureReplayData()->NewFinalizationRegistryId();
  recordreplay::Assert("FinalizationRegistry.adopt %d", id);
  registry->set_record_replay_id(id);
  return true;
}

void ReplayFinalizationRegistries::OnRegister(
    i::Isolate* isolate, i::Handle<i::JSFinalizationRegistry> registry,
    i::Handle<i::WeakCell> cell) {
  if (!Adopt(isolate, registry)) return;

  if (recordreplay::HasDivergedFromRecording()) {
    cell->set_record_replay_id(kUndeliverableCellId);
    return;
  }

  // The recording cannot describe when this cell is cleared, and the registry's
  // other cells are delivered from the recording.
  CHECK_WITH_MSG(
      AreEventsAvailable(),
      "FinalizationRegistry.prototype.register on a tracked registry at a "
      "non-deterministic point");

  ReplayIsolateData* data = isolate->EnsureReplayData();
  int id = data->NewWeakCellId();
  recordreplay::Assert("FinalizationRegistry.register %d %d",
                       registry->record_replay_id(), id);
  cell->set_record_replay_id(id);
  data->set_has_registered_weak_cells();

  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8::Local<v8::Value> local = Utils::ToLocal(i::Handle<i::JSObject>(registry));

  if (!recordreplay::IsReplaying()) {
    auto& recorded = data->recorded_finalization_registries();
    if (!recorded.count(registry->record_replay_id())) {
      auto entry =
          std::make_unique<ReplayIsolateData::RecordedFinalizationRegistry>();
      entry->data = data;
      entry->id = registry->record_replay_id();
      entry->registry.Reset(v8_isolate, local);
      entry->registry.SetWeak(entry.get(), OnRecordedRegistryCollected,
                              v8::WeakCallbackType::kParameter);
      recorded.emplace(entry->id, std::move(entry));
    }
    return;
  }

  i::Handle<i::SimpleNumberDictionary> cells;
  if (registry->record_replay_cells().IsUndefined(isolate)) {
    cells = i::SimpleNumberDictionary::New(isolate, 1);
    // The recording can deliver callbacks of a registry which is no longer
    // reachable, as long as the recording's GC did not collect it.
    data->retained_finalization_registries().emplace(
        registry->record_replay_id(), v8::Global<v8::Value>(v8_isolate, local));
  } else {
    cells = i::handle(
        i::SimpleNumberDictionary::cast(registry->record_replay_cells()),
        isolate);
  }
  cells = i::SimpleNumberDictionary::Set(isolate, cells, id, cell);
  registry->set_record_replay_cells(*cells);
}

void ReplayFinalizationRegistries::OnUnregisterCell(
    i::Isolate* isolate, i::JSFinalizationRegistry registry, i::WeakCell cell) {
  if (!cell.record_replay_id() || recordreplay::HasDivergedFromRecording())
    return;

  // The other side would still have this cell registered, and deliver it or
  // look for it according to the recording.
  CHECK_WITH_MSG(
      AreEventsAvailable(),
      "FinalizationRegistry.prototype.unregister on a tracked registry at a "
      "non-deterministic point");

  if (registry.record_replay_cells().IsUndefined(isolate)) return;
  i::SimpleNumberDictionary cells =
      i::SimpleNumberDictionary::cast(registry.record_replay_cells());
  i::InternalIndex entry = cells.FindEntry(isolate, cell.record_replay_id());
  if (entry.is_found()) {
    cells.ClearEntry(entry);
    cells.ElementRemoved();
  }
}

bool ReplayFinalizationRegistries::NextCell(
    i::Isolate* isolate, i::Handle<i::JSFinalizationRegistry> registry) {
  if (!registry->record_replay_id()) return true;
  if (!AreEventsAvailable()) return false;

  uintptr_t id = 0;
  if (recordreplay::IsRecording() && registry->NeedsCleanup()) {
    int cell_id =
        i::WeakCell::cast(registry->cleared_cells()).record_replay_id();
    CHECK_GT(cell_id, 0);
    id = cell_id;
  }
  id = recordreplay::RecordReplayValue("FinalizationRegistry.cleanup", id);
  if (!id) return false;

  if (recordreplay::IsReplaying()) {
    CHECK_WITH_MSG(!registry->record_replay_cells().IsUndefined(isolate),
                   "Recorded FinalizationRegistry cleanup for unknown cell");
    i::Handle<i::SimpleNumberDictionary> cells = i::handle(
        i::SimpleNumberDictionary::cast(registry->record_replay_cells()),
        isolate);
    i::InternalIndex entry =
        cells->FindEntry(isolate, static_cast<uint32_t>(id));
    CHECK_WITH_MSG(entry.is_found(),
                   "Recorded FinalizationRegistry cleanup for unknown cell");
    i::WeakCell cell = i::WeakCell::cast(cells->ValueAt(entry));
    // Nullify assumes an active cell, and the replay's GC clearing a tracked
    // cell on its own would otherwise go unnoticed.
    CHECK_WITH_MSG(!cell.target().IsUndefined(isolate),
                   "Tracked FinalizationRegistry cell cleared while replaying");
    // Do what the recording's GC did when it found the target dead: this moves
    // the cell to the head of the cleared list, where the cleanup loop pops it.
    cell.Nullify(isolate, [](i::HeapObject, i::ObjectSlot, i::Object) {});
    cells = i::SimpleNumberDictionary::DeleteEntry(isolate, cells, entry);
    registry->set_record_replay_cells(*cells);
  }
  return true;
}

bool ReplayFinalizationRegistries::ShouldPostCleanupTask(i::Isolate* isolate) {
  return !isolate->replay_data()
              ->is_finalization_registry_cleanup_task_posted() &&
         isolate->heap()->RecordReplayHasDirtyJSFinalizationRegistries(
             i::Heap::RecordReplayTracking::kTracked);
}

void ReplayFinalizationRegistries::PostCleanupTask(i::Isolate* isolate) {
  isolate->replay_data()->set_is_finalization_registry_cleanup_task_posted(
      true);
  auto taskrunner = i::V8::GetCurrentPlatform()->GetForegroundTaskRunner(
      reinterpret_cast<v8::Isolate*>(isolate));
  taskrunner->PostNonNestableTask(
      std::make_unique<i::FinalizationRegistryCleanupTask>(
          isolate->heap(), i::Heap::RecordReplayTracking::kTracked));
}

void ReplayFinalizationRegistries::TakeCollected(i::Isolate* isolate,
                                                 std::vector<int>* ids) {
  ReplayIsolateData* data = isolate->replay_data();
  ids->swap(data->collected_finalization_registries());
  for (int id : *ids) data->recorded_finalization_registries().erase(id);
}

void ReplayFinalizationRegistries::ReleaseCollected(
    i::Isolate* isolate, const std::vector<int>& ids) {
  // The recording cannot deliver from these registries anymore.
  ReplayIsolateData* data = isolate->replay_data();
  for (int id : ids) data->retained_finalization_registries().erase(id);
}

i::MaybeHandle<i::JSFinalizationRegistry>
ReplayFinalizationRegistries::TakeRegistryForTask(i::Isolate* isolate) {
  isolate->replay_data()->set_is_finalization_registry_cleanup_task_posted(
      false);
  if (!AreEventsAvailable()) return {};

  i::MaybeHandle<i::JSFinalizationRegistry> registry;
  uintptr_t id = 0;
  if (recordreplay::IsRecording()) {
    registry = isolate->heap()->RecordReplayDequeueDirtyJSFinalizationRegistry(
        i::Heap::RecordReplayTracking::kTracked);
    if (!registry.is_null())
      id = registry.ToHandleChecked()->record_replay_id();
  }
  id = recordreplay::RecordReplayValue("FinalizationRegistry.task", id);
  if (recordreplay::IsReplaying() && id) {
    registry = LookupRegistry(isolate, static_cast<int>(id));
  }
  return registry;
}

}  // namespace replayio
}  // namespace v8
