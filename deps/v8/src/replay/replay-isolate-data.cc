#include "src/replay/replay-isolate-data.h"

#include "src/handles/global-handles.h"

namespace v8 {
namespace replayio {

ReplayIsolateData::~ReplayIsolateData() {
  if (tracked_weak_refs_location_) {
    internal::GlobalHandles::Destroy(tracked_weak_refs_location_);
  }
}

}  // namespace replayio
}  // namespace v8
