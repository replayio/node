#ifndef SRC_NODE_DEPENDENCY_GRAPH_H_
#define SRC_NODE_DEPENDENCY_GRAPH_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "v8.h"

namespace node {

class AsyncWrap;
class ExternalReferenceRegistry;
class IsolateData;

// Why a requestEnd reports no bytes of its body (bodyDropped).
enum class RequestBodyDrop {
  kTooLong,  // "too-long": longer than requestEnd reports
  kBudget,   // "budget": the process's body bytes are spent
};

// What this build reports in the dependency graph, beyond async ops, event
// dispatches and Web Locks. The root marker lists all of them (features), so a
// reader tells a build without one from a graph without its nodes.
enum class DependencyGraphFeature {
  kRequestStart,        // "requestStart": requestStart{method,url} dispatches,
                        // request edges to their responses' nodes
  kNetworkRequest,      // "networkRequest": networkRequest{requestId}
  kNetworkReceiveData,  // "networkReceiveData": answer parts handed to a
                        // client in networkReceiveData dispatches
  kRequestHeaders,      // "requestHeaders": requestStart{headers,
                        // headersCapped,headersUnread}
  kRequestBody,         // "requestBody": requestEnd{length,body|bodyDropped}
  kRequestHeaderNames,  // "requestHeaderNames": requestStart{headerNames}
  kRequestParts,        // "requestParts": requestData and requestEnd
                        // dispatches
  kFsOps,               // "fsOps": fsOp and fsDone (InstallFsDependencyGraph),
                        // the root marker's cwd. Listed only when reported.
  kReactionJobs,        // "reactionJobs": promiseReactionJob{reaction} with
                        // reactionSettle and reactionAttach edges (V8)
  kCount,              // not a feature: their number
};

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
// An HTTP server request (lib/internal/recordreplay/dependency_graph_http.js)
// is dispatched in a DependencyExecution of a requestStart{method,url,headers,
// headerNames,headersCapped,headersUnread} node, ended as other dispatches. So
// is the delivery of an outbound request's answer to its client
// (lib/internal/recordreplay/network.js), in a
// networkReceiveData{requestId,length,part,timedBy} node. So is the hand-over
// of each part of a server request's message to the program
// (lib/_http_common.js): a body chunk's in a requestData{length} node, the
// complete message's in a requestEnd{length,body|bodyDropped} node.
//
// Every method returns at once unless
// v8::recordreplay::DependencyGraphEnabled(): the map, the windows, the
// dispatches and the request bodies are only kept while it holds.
class AsyncDependencyGraph {
 public:
  // The last init of an id wins.
  void OnInit(v8::Isolate* isolate,
              double async_id,
              v8::Local<v8::String> type);
  void OnInitTimer(double async_id, double duration, bool single_shot);
  void OnInitMicrotask(double async_id);
  // The root marker: marker{reason:ExecuteScript,name:StartExecution,features}.
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
  // are reported empty. raw_headers: IncomingMessage#rawHeaders, of which only
  // own string elements are read; header_element_limit: the parser's
  // maxHeaderPairs, a count of rawHeaders elements. wire_ids: the
  // x-replay-wire-id values the parser removed from rawHeaders.
  int OnBeginRequest(v8::Isolate* isolate,
                     v8::Local<v8::Value> method,
                     v8::Local<v8::Value> url,
                     v8::Local<v8::Value> raw_headers,
                     v8::Local<v8::Value> header_element_limit,
                     v8::Local<v8::Value> wire_ids);
  // A chunk of the body of request (its node; 0: none), as received.
  void OnRequestBodyBytes(int request, v8::Local<v8::ArrayBufferView> chunk);
  // The hand-over of a body chunk of length bytes begins: a dispatch in a
  // requestData node with a request edge from request. A dispatch without a
  // node if request is 0, so its end always pairs.
  void OnBeginRequestData(int request, size_t length);
  // The request's message is complete, and the hand-over of its end begins: a
  // dispatch, as a chunk's, in a requestEnd node. Its body: all bytes, base64,
  // or why none (bodyDropped).
  void OnBeginRequestEnd(int request);
  // The request's parser is freed: its message, if not complete, never will
  // be. Frees its body.
  void OnRequestAbandoned(int request);
  // request_id: the request's network data id. length: the part's bytes.
  // part, timed_by: lib/internal/recordreplay/network.js beginDelivery.
  void OnBeginReceiveData(v8::Isolate* isolate,
                          v8::Local<v8::String> request_id,
                          double length,
                          v8::Local<v8::String> part,
                          v8::Local<v8::String> timed_by);
  // Ends the innermost dispatch not ended yet.
  void OnEndDispatch();
  // The node the id's last init added, 0 if none.
  int InitNode(double async_id) const;

  // The JS bindings, on internalBinding('async_wrap').
  static void CreatePerIsolateProperties(IsolateData* isolate_data,
                                         v8::Local<v8::ObjectTemplate> target);
  static void RegisterExternalReferences(ExternalReferenceRegistry* registry);

 private:
  struct Op {
    int node;
    AsyncOp op;
  };

  struct RequestBody {
    uint64_t length = 0;
    // All bytes so far, unless dropped.
    std::string bytes;
    // Its bytes were dropped: too long, or past the budget.
    bool dropped = false;
  };

  void AddOp(double async_id, AsyncOp op, const std::string& json);
  void PushDispatch(int node);
  // Takes the request's body.
  int NewRequestEndNode(int request);
  // Frees the body's bytes, and their part of body_bytes_.
  void DropBody(RequestBody& body);
  // Drops the innermost dispatches an unwind already ended.
  void DropEndedDispatches();

  std::unordered_map<double, Op> ops_;
  // Per stack slot: the window's execution, or nullopt if it didn't begin.
  std::vector<std::optional<DependencyExecution>> windows_;
  // Per dispatch not ended, innermost last: its execution, or nullopt if it
  // didn't begin.
  std::vector<std::optional<DependencyExecution>> dispatches_;
  // Per request node whose message is not complete and not abandoned.
  std::unordered_map<int, RequestBody> request_bodies_;
  // The body bytes in request_bodies_ and in requestEnd nodes: at most
  // kRequestBodyBudgetBytes.
  uint64_t body_bytes_ = 0;
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

// fs ops in the Replay dependency graph: every libuv fs request the main thread
// posts (uv_fs_set_observers), sync or async, adds an fsOp{op,sync,path,
// rawPath,newPath,rawNewPath,target,fd,fdPath,flags,otherFlags,offset,length}
// node. An async one's completion, before its callback, adds an
// fsDone{result|error,path,size,modifiedTime,inode,device,fileType} node with
// an fsCompletion edge from its fsOp, which has an fsOperation edge from its
// ReqWrap's asyncInit, and an fsCallback edge to the callback window it
// dispatches. A sync one's fsOp carries the fsDone fields. Paths only, never
// contents. fs work that posts no main-thread libuv request reports itself as
// an FsJob, with the same nodes. Not covered: fs syscalls outside libuv and
// FsJobs, e.g. of native addons. Not recorded: what queued a StatWatcher's
// polls, the path of a readdir or closedir. Not reported: the recorder's own
// requests, between dependencyGraphBeginRecorderIO and
// dependencyGraphEndRecorderIO, and requests posted while events are disallowed
// (GC).
//
// Reported while v8::recordreplay::DependencyGraphEnabled(), unless the driver
// disables the dependency-graph-fs feature: it changes no JS behavior, so a
// replay can turn it off. Call before any fs request is posted.
void InstallFsDependencyGraph();
// fd's close may not be an fs request the main thread posts: its ops report no
// fdPath.
void OnFsFdHandedOver(int fd);
// The process cwd fsOp paths resolve against: read, recorded, while
// v8::recordreplay::DependencyGraphInstrumented(), at the first checkpoint and
// after each chdir.
void RecordFsDependencyGraphCwd();

// fs work an fsOp reports as one op, though it posts no main-thread libuv
// request: threadpool jobs, std::filesystem bindings, chdir.
enum class FsJob {
  kReadFile,          // "readFile": ReadFileJob
  kWriteFile,         // "writeFile": WriteFileJob
  kReadDirRecursive,  // "readdirRecursive": readdirRecursive
  kCpCheckPaths,      // "cpCheckPaths": CpSyncCheckPaths
  kCpFile,            // "cpFile": CpSyncOverrideFile
  kCpDir,             // "cpDir": CpSyncCopyDir, CpDirJob
  kRm,                // "rm": RmSync
  kChdir,             // "chdir": process.chdir
};

// What an FsJob reports when issued. The views outlive the call.
struct FsJobIssue {
  FsJob job;
  std::string_view path;
  // The destination (cp).
  std::optional<std::string_view> new_path;
  std::optional<int> flags;
  std::optional<uint64_t> length;
};

// What an FsJob reports when done.
struct FsJobResult {
  // A uv error code, 0 if none or none known.
  int error = 0;
  // The step whose error it is, e.g. "open" of a readFile.
  const char* error_op = nullptr;
  // It failed without a uv error code, e.g. by an exception.
  bool failed = false;
  // Bytes, entries.
  std::optional<int64_t> result;
  // readFile: the fd it handed back to read in chunks.
  std::optional<int> fd;
};

// An async FsJob of wrap (its asyncInit has the fsOperation edge). Returns its
// fsOp node for FsJobDoneScope, 0 if none.
int OnFsJobIssue(AsyncWrap* wrap, const FsJobIssue& issue);

// The completion of an async FsJob, in the scope its callback runs in. node:
// OnFsJobIssue's; issue: the same.
class FsJobDoneScope {
 public:
  FsJobDoneScope(int node, const FsJobIssue& issue, const FsJobResult& result);
  ~FsJobDoneScope();
  FsJobDoneScope(const FsJobDoneScope&) = delete;
  FsJobDoneScope& operator=(const FsJobDoneScope&) = delete;
};

// A sync FsJob: one fsOp.
void OnFsJobSync(const FsJobIssue& issue, const FsJobResult& result);

// A sync FsJob for the lifetime of a binding's scope: failed if the binding
// leaves an exception pending.
class FsJobSyncScope {
 public:
  FsJobSyncScope(v8::Isolate* isolate, const FsJobIssue& issue)
      : isolate_(isolate), issue_(issue) {}
  ~FsJobSyncScope() {
    if (!v8::recordreplay::DependencyGraphEnabled()) return;
    OnFsJobSync(issue_, {.failed = isolate_->HasPendingException()});
  }
  FsJobSyncScope(const FsJobSyncScope&) = delete;
  FsJobSyncScope& operator=(const FsJobSyncScope&) = delete;

 private:
  v8::Isolate* isolate_;
  FsJobIssue issue_;
};

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_DEPENDENCY_GRAPH_H_
