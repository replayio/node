#ifndef V8_REPLAY_DEPENDENCY_GRAPH_H_
#define V8_REPLAY_DEPENDENCY_GRAPH_H_

#include "include/v8-promise.h"
#include "src/common/globals.h"
#include "src/handles/handles.h"

namespace v8 {
namespace internal {

class JSPromise;
class JSReceiver;

// Reports promises to the Replay dependency graph (recordreplay::
// NewDependencyGraphNode etc.): promise creation adds newPromise nodes with
// parentPromise edges, resolution adds promiseSettled nodes with basePromise
// edges, adopting a thenable adds adoptedPromise edges, and promise reaction
// jobs run inside a dependency execution of the promise's node.
class RecordReplayDependencyGraph : public AllStatic {
 public:
  // Called once by recordreplay::SetRecordingOrReplaying, before any isolate
  // is created.
  static void Initialize();

  // Whether OnPromiseHook and OnPromiseAdoption are called. Besides while
  // recordreplay::DependencyGraphEnabled(), this is while JS value asserts are
  // on: they include the object IDs the calls assign to promises, so the calls
  // have to happen at the same places when recording and replaying.
  static bool ReportsPromises();

  // Whether graph updates are asserted (RECORD_REPLAY_DEPENDENCY_GRAPH_ASSERTS).
  // The updates then happen while recording too, and the promise IDs in them
  // have to match the recording's.
  static bool AssertsUpdates();

  // Precondition for both: ReportsPromises().
  static void OnPromiseHook(Isolate* isolate, PromiseHookType type,
                            DirectHandle<JSPromise> promise,
                            DirectHandle<Object> parent);
  static void OnPromiseAdoption(Isolate* isolate,
                                DirectHandle<JSPromise> promise,
                                DirectHandle<JSReceiver> adopted);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_REPLAY_DEPENDENCY_GRAPH_H_
