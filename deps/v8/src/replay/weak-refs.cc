#include "src/replay/weak-refs.h"

#include "include/replayio.h"
#include "src/api/api-inl.h"
#include "src/execution/isolate.h"
#include "src/objects/heap-object.h"
#include "src/replay/replay-isolate-data.h"

namespace v8 {
namespace replayio {

void ReplayWeakRefPins::Pin(internal::Isolate* isolate,
                            internal::Tagged<internal::HeapObject> target) {
  DCHECK(!recordreplay::IsReplaying());
  auto& entries = isolate->EnsureReplayData()->weak_ref_pins();
  auto local = Utils::ToLocal(internal::Handle<internal::Object>(target, isolate));
  for (auto& entry : entries) {
    if (entry == local) return;
  }
  entries.emplace_back(reinterpret_cast<v8::Isolate*>(isolate), local);
}

void ReplayWeakRefPins::Unpin(internal::Isolate* isolate,
                              internal::Tagged<internal::HeapObject> target) {
  ReplayIsolateData* data = isolate->replay_data();
  if (!data) return;
  auto& entries = data->weak_ref_pins();
  auto local = Utils::ToLocal(internal::Handle<internal::Object>(target, isolate));
  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (*it == local) {
      entries.erase(it);
      return;
    }
  }
}

}  // namespace replayio
}  // namespace v8
