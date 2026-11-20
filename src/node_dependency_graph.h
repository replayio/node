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

// A dependency execution of a node (v8::recordreplay::BeginDependencyExecution).
// It saves the dependency execution depth when it begins, and ends by
// unwinding to it: that also ends executions begun in it whose own end was
// skipped, such as those of promise reaction jobs a termination escaped from.
class DependencyExecution {
 public:
  // nullopt for node 0: no node, e.g. before the first checkpoint.
  static std::optional<DependencyExecution> Begin(int node);
  size_t depth() const { return depth_; }
  // Whether an unwind past its depth already ended it.
  bool ended() const;
  void End() const;

 private:
  explicit DependencyExecution(size_t depth) : depth_(depth) {}
  size_t depth_;
};

// Async ops (async_hooks) in the Replay dependency graph
// (v8::recordreplay::NewDependencyGraphNode etc.). An op's init adds a node.
// Each callback of the op runs in a window, push_async_context ...
// pop_async_context, which is a DependencyExecution of the op's run node.
//
// Windows mirror AsyncHooks' async id stack by slot. Shrinking the stack ends
// the outermost window it drops, and with it the windows and dispatches above
// it, also those a clear of the stack dropped without a pop.
//
// Ids without an init (0, PROMISE ids) and ids whose init came before the first
// checkpoint get no window.
//
// Event dispatches (EventEmitter#emit, EventTarget[kHybridDispatch]) call their
// listeners in a DependencyExecution of an emitEvent{type} node. Dispatches
// nest. Not dispatchEvent: the backend reads that kind as DOM input.
//
// An HTTP server request (lib/_http_server.js) is dispatched in a
// DependencyExecution of a requestStart{method,url} node, ended as other
// dispatches.
//
// Every method returns at once unless
// v8::recordreplay::DependencyGraphEnabled(): the map, the windows and the
// dispatches are only kept while it holds.
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
  // type: the event type. Reported: a string, or a Symbol's description.
  // Other values are not converted, which could run JS.
  void OnBeginDispatch(v8::Isolate* isolate, v8::Local<v8::Value> type);
  // Returns the request's node, 0 if none. method and url other than strings
  // are reported empty.
  int OnBeginRequest(v8::Isolate* isolate,
                     v8::Local<v8::Value> method,
                     v8::Local<v8::Value> url);
  // Ends the innermost dispatch not ended yet.
  void OnEndDispatch();

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
  void PushDispatch(int node);
  // Drops the innermost dispatches an unwind already ended.
  void DropEndedDispatches();

  std::unordered_map<double, Op> ops_;
  // Per stack slot: the window's execution, or nullopt if it didn't begin.
  std::vector<std::optional<DependencyExecution>> windows_;
  // Per dispatch not ended, innermost last: its execution, or nullopt if it
  // didn't begin.
  std::vector<std::optional<DependencyExecution>> dispatches_;
};

// A DependencyExecution for the lifetime of a C++ scope.
class DependencyExecutionScope {
 public:
  explicit DependencyExecutionScope(int node)
      : execution_(DependencyExecution::Begin(node)) {}
  ~DependencyExecutionScope() {
    if (execution_) execution_->End();
  }
  DependencyExecutionScope(const DependencyExecutionScope&) = delete;
  DependencyExecutionScope& operator=(const DependencyExecutionScope&) = delete;

 private:
  std::optional<DependencyExecution> execution_;
};

// Web Locks (navigator.locks) in the Replay dependency graph, with the kinds of
// Blink's LockManager: a request adds a lockManagerRequest{name} node, and its
// grant runs in a DependencyExecutionScope of a lockRequestGranted node, with a
// creator edge from the request. Both return 0 unless
// v8::recordreplay::DependencyGraphEnabled().
int NewLockRequestDependencyNode(v8::Isolate* isolate,
                                 v8::Local<v8::String> name);
int NewLockGrantedDependencyNode(int request_node);

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_DEPENDENCY_GRAPH_H_
