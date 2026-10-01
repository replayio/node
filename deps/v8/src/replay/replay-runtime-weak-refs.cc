#include "include/replayio.h"
#include "src/execution/arguments-inl.h"
#include "src/objects/js-weak-refs-inl.h"
#include "src/replay/finalization-registry.h"
#include "src/runtime/runtime-utils.h"

namespace v8 {
namespace internal {

// Called from WeakRef.prototype.deref(). Records/replays target liveness so
// the result is deterministic. When replaying, the GC keeps the target alive
// (see MarkingVisitorBase::VisitJSWeakRef) until it is observed dead here.
RUNTIME_FUNCTION(Runtime_JSReplayWeakRefDeref) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<JSWeakRef> weak_ref = args.at<JSWeakRef>(0);

  Object target = weak_ref->target();
  const bool had_target = !target.IsUndefined(isolate);
  uintptr_t alive = had_target ? 1 : 0;
  uintptr_t recorded_alive =
      recordreplay::RecordReplayValue("JSWeakRef.deref", alive);

  if (!recorded_alive && alive) {
    // Can only happen during replay: The target is still alive but at recording
    // time it was dead.
    DCHECK(recordreplay::IsReplaying());
    weak_ref->set_target(ReadOnlyRoots(isolate).undefined_value());
    return ReadOnlyRoots(isolate).undefined_value();
  }

  return target;
}

RUNTIME_FUNCTION(Runtime_JSReplayFinalizationRegistryConstruct) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  replayio::ReplayFinalizationRegistries::OnConstruct(
      isolate, args.at<JSFinalizationRegistry>(0));
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_JSReplayFinalizationRegistryRegister) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  replayio::ReplayFinalizationRegistries::OnRegister(
      isolate, args.at<JSFinalizationRegistry>(0), args.at<WeakCell>(1));
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_JSReplayFinalizationRegistryNextCell) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  return isolate->heap()->ToBoolean(
      replayio::ReplayFinalizationRegistries::NextCell(
          isolate, args.at<JSFinalizationRegistry>(0)));
}

}  // namespace internal
}  // namespace v8
