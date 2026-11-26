#ifndef V8_REPLAY_DEPENDENCY_GRAPH_H_
#define V8_REPLAY_DEPENDENCY_GRAPH_H_

#include "include/v8-promise.h"
#include "src/common/globals.h"
#include "src/handles/handles.h"

namespace v8 {
namespace internal {

class JSPromise;
class JSReceiver;
class PromiseReactionJobTask;

// Reports promises to the Replay dependency graph (recordreplay::
// NewDependencyGraphNode etc.): promise creation adds newPromise nodes with
// parentPromise edges, resolution adds promiseSettled nodes with basePromise
// edges, adopting a thenable adds adoptedPromise edges, enqueuing a reaction
// job adds a promiseReactionJob node, and promise jobs run inside a dependency
// execution of their job's node or, without one, of the promise's node.
class RecordReplayDependencyGraph : public AllStatic {
 public:
  // Called once by recordreplay::SetRecordingOrReplaying, before any isolate
  // is created.
  static void Initialize();

  // Whether the calls below are made. Besides while
  // recordreplay::DependencyGraphEnabled(), this is while JS value asserts are
  // on: they include the object IDs the calls assign to promises, so the calls
  // have to happen at the same places when recording and replaying.
  static bool ReportsPromises();

  // Whether graph updates are asserted (RECORD_REPLAY_DEPENDENCY_GRAPH_ASSERTS).
  // Recordings of an instrumented graph then update it too, and the promise IDs
  // in the asserted updates have to match the recording's.
  static bool AssertsUpdates();

  // Precondition for all: ReportsPromises().
  static void OnPromiseHook(Isolate* isolate, PromiseHookType type,
                            DirectHandle<JSPromise> promise,
                            DirectHandle<Object> parent);
  static void OnPromiseAdoption(Isolate* isolate,
                                DirectHandle<JSPromise> promise,
                                DirectHandle<JSReceiver> adopted);

  // At the settle of a promise, before its reaction jobs are enqueued. Returns
  // the node ID of the settle which triggers the promise's reactions: its own,
  // or that of the thenable it adopted; 0 if not recorded.
  static int OnPromiseSettle(Isolate* isolate, DirectHandle<JSPromise> promise);

  // The value OnPromiseSettle returned for the settled promise.
  static int ReactionSettleNodeId(Isolate* isolate,
                                  DirectHandle<JSPromise> settled);

  // At the enqueue of a reaction job, in the execution of the later of its
  // settle and its attach: adds its promiseReactionJob node, with
  // reactionSettle and reactionAttach edges, as the range of its derived
  // promise's next kBefore hook. reaction_settle_node_id: as OnPromiseSettle.
  static void OnReactionJob(Isolate* isolate,
                            DirectHandle<PromiseReactionJobTask> task,
                            int reaction_settle_node_id);

  // Without precondition: the number of promise jobs between their kBefore and
  // kAfter hooks, and the unwind to such a number. A termination escaping
  // MicrotaskQueue::RunMicrotasks skips the kAfter hooks of its running jobs;
  // RunMicrotasks unwinds them to its entry depth.
  static size_t RunningPromiseJobDepth();
  static void UnwindRunningPromiseJobs(size_t depth);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_REPLAY_DEPENDENCY_GRAPH_H_
