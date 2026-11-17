#ifndef SRC_NODE_DEPENDENCY_GRAPH_H_
#define SRC_NODE_DEPENDENCY_GRAPH_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "v8.h"

namespace node {

class ExternalReferenceRegistry;
class IsolateData;

// The classes of async op in the Replay dependency graph: the node an op's
// init adds, and the node each of its callbacks runs in, with the edge
// between them. Timers and microtasks use the kinds Chromium reports them
// with.
enum class AsyncOp {
  kGeneric,    // asyncInit{type} -scheduler-> asyncCallback
  kTimer,      // timerScheduled{duration,singleShot} -baseTimer-> timerFired
  kMicrotask,  // enqueueMicrotask -scheduler-> runMicrotask
  kScript,     // marker{ExecuteScript}; the callback runs in it
};

// Async ops (async_hooks) in the Replay dependency graph
// (v8::recordreplay::NewDependencyGraphNode etc.). An op's init adds a node.
// Each callback of the op runs in a window, push_async_context ...
// pop_async_context, which is a dependency execution of the op's run node.
//
// Windows mirror AsyncHooks' async id stack by slot. A window saves the
// dependency execution depth when it begins, and shrinking the stack unwinds
// to it: that also ends executions whose own end was skipped, such as those of
// promise reaction jobs a termination escaped from, or of windows a clear of
// the stack dropped.
//
// Ids without an init (0, PROMISE ids) and ids whose init came before the first
// checkpoint get no window. Every method returns at once unless
// v8::recordreplay::DependencyGraphEnabled(): the map is only kept when
// replaying.
class AsyncDependencyGraph {
 public:
  // The last init of an id wins.
  void OnInit(v8::Isolate* isolate,
              double async_id,
              v8::Local<v8::String> type);
  void OnInitTimer(double async_id, double duration, bool single_shot);
  void OnInitMicrotask(double async_id);
  void OnInitScript(double async_id);
  void OnDestroy(double async_id);
  // offset: the window's stack slot, the stack length before the push.
  void OnPush(double async_id, uint32_t offset);
  // length: the stack length after a pop or a clear.
  void OnPop(uint32_t length);

  // The JS bindings, on internalBinding('async_wrap').
  static void CreatePerIsolateProperties(IsolateData* isolate_data,
                                         v8::Local<v8::ObjectTemplate> target);
  static void RegisterExternalReferences(ExternalReferenceRegistry* registry);

 private:
  struct Op {
    int node;
    AsyncOp op;
  };

  void AddOp(double async_id, AsyncOp op, const std::string& json);

  std::unordered_map<double, Op> ops_;
  // Per stack slot: the dependency execution depth before the window began,
  // or nullopt if it didn't.
  std::vector<std::optional<size_t>> windows_;
};

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_DEPENDENCY_GRAPH_H_
