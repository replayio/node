#include "node_dependency_graph.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#include "env-inl.h"
#include "json_utils.h"
#include "node_external_reference.h"
#include "util-inl.h"

namespace node {

using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::ObjectTemplate;
using v8::String;
using v8::Symbol;
using v8::Value;

namespace {

// A node's JSON: {"kind":kind, ...fields}.
template <typename... Fields>
std::string NodeJson(std::string_view kind, const Fields&... fields) {
  std::ostringstream out;
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  JSONWriter writer(out, true);
  writer.json_start();
  writer.json_keyvalue("kind", kind);
  (writer.json_keyvalue(fields.first, fields.second), ...);
  writer.json_end();
  return out.str();
}

// The node a callback of the op runs in: a new node with an edge from the
// init node, or for kScript the init node itself.
int RunNode(int init_node, AsyncOp op) {
  auto add = [init_node](const char* run, const char* edge) {
    int node = v8::recordreplay::NewDependencyGraphNode(run);
    v8::recordreplay::AddDependencyGraphEdge(init_node, node, edge);
    return node;
  };
  switch (op) {
    case AsyncOp::kGeneric:
      return add(R"({"kind":"asyncCallback"})", R"({"kind":"scheduler"})");
    case AsyncOp::kTimer:
      return add(R"({"kind":"timerFired"})", R"({"kind":"baseTimer"})");
    case AsyncOp::kMicrotask:
      return add(R"({"kind":"runMicrotask"})", R"({"kind":"scheduler"})");
    case AsyncOp::kScript:
      return init_node;
  }
  UNREACHABLE();
}

// Other values are not converted, which could run JS.
Local<Value> StringOrEmpty(Isolate* isolate, Local<Value> value) {
  return value->IsString() ? value : String::Empty(isolate).As<Value>();
}

AsyncDependencyGraph& GetCurrent(const FunctionCallbackInfo<Value>& args) {
  return Environment::GetCurrent(args)->async_hooks()->dependency_graph();
}

double NumberArg(const FunctionCallbackInfo<Value>& args, int index) {
  CHECK(args[index]->IsNumber());
  return args[index].As<Number>()->Value();
}

void Instrumented(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(v8::recordreplay::DependencyGraphInstrumented());
}

void Init(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[1]->IsString());
  GetCurrent(args).OnInit(
      args.GetIsolate(), NumberArg(args, 0), args[1].As<String>());
}

void InitTimer(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[2]->IsBoolean());
  GetCurrent(args).OnInitTimer(
      NumberArg(args, 0), NumberArg(args, 1), args[2]->IsTrue());
}

void InitMicrotask(const FunctionCallbackInfo<Value>& args) {
  GetCurrent(args).OnInitMicrotask(NumberArg(args, 0));
}

void Destroy(const FunctionCallbackInfo<Value>& args) {
  GetCurrent(args).OnDestroy(NumberArg(args, 0));
}

void Push(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[1]->IsUint32());
  GetCurrent(args).OnPush(NumberArg(args, 0),
                          args[1].As<v8::Uint32>()->Value());
}

void Pop(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsUint32());
  GetCurrent(args).OnPop(args[0].As<v8::Uint32>()->Value());
}

void BeginDispatch(const FunctionCallbackInfo<Value>& args) {
  GetCurrent(args).OnBeginDispatch(args.GetIsolate(), args[0]);
}

void EndDispatch(const FunctionCallbackInfo<Value>& args) {
  GetCurrent(args).OnEndDispatch();
}

void BeginRequest(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(GetCurrent(args).OnBeginRequest(
      args.GetIsolate(), args[0], args[1]));
}

void BeginReceiveData(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString());
  GetCurrent(args).OnBeginReceiveData(
      args.GetIsolate(), args[0].As<String>(), NumberArg(args, 1));
}

int RequestArg(const FunctionCallbackInfo<Value>& args, int index) {
  CHECK(args[index]->IsInt32());
  return args[index].As<v8::Int32>()->Value();
}

void NewResponseNode(int request, const std::string& json) {
  v8::recordreplay::AddDependencyGraphEdge(
      request,
      v8::recordreplay::NewDependencyGraphNode(json.c_str()),
      R"({"kind":"request"})");
}

// A server response (http.ServerResponse): responseStart{status} at its head,
// responseData{length} per body write and responseEnd at its end. Each is made
// in the JS that writes it, which is its cause, with a request edge from its
// request's node (0: none).
void ResponseStart(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsInt32());
  int request = RequestArg(args, 1);
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  NewResponseNode(
      request,
      NodeJson("responseStart",
               std::pair{"status", args[0].As<v8::Int32>()->Value()}));
}

void ResponseData(const FunctionCallbackInfo<Value>& args) {
  double length = NumberArg(args, 0);
  int request = RequestArg(args, 1);
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  NewResponseNode(request,
                  NodeJson("responseData", std::pair{"length", length}));
}

void ResponseEnd(const FunctionCallbackInfo<Value>& args) {
  int request = RequestArg(args, 0);
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  NewResponseNode(request, R"({"kind":"responseEnd"})");
}

// An outbound request of the recording's network data
// (lib/internal/recordreplay/network.js): networkRequest{requestId} with its
// Network request id, made in the JS that issues it.
void NetworkRequest(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString());
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  Utf8Value id(args.GetIsolate(), args[0]);
  v8::recordreplay::NewDependencyGraphNode(
      NodeJson("networkRequest", std::pair{"requestId", id.ToStringView()})
          .c_str());
}

}  // namespace

std::optional<DependencyExecution> DependencyExecution::Begin(int node) {
  if (!node) return std::nullopt;
  DependencyExecution execution(v8::recordreplay::DependencyExecutionDepth());
  v8::recordreplay::BeginDependencyExecution(node);
  return execution;
}

bool DependencyExecution::ended() const {
  return depth_ >= v8::recordreplay::DependencyExecutionDepth();
}

void DependencyExecution::End() const {
  v8::recordreplay::UnwindDependencyExecutions(depth_);
}

void AsyncDependencyGraph::AddOp(double async_id,
                                 AsyncOp op,
                                 const std::string& json) {
  int node = v8::recordreplay::NewDependencyGraphNode(json.c_str());
  if (node) {
    ops_.insert_or_assign(async_id, Op{node, op});
  } else {
    ops_.erase(async_id);
  }
}

void AsyncDependencyGraph::OnInit(Isolate* isolate,
                                  double async_id,
                                  Local<String> type) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  Utf8Value type_utf8(isolate, type);
  AddOp(async_id,
        AsyncOp::kGeneric,
        NodeJson("asyncInit", std::pair{"type", type_utf8.ToStringView()}));
}

void AsyncDependencyGraph::OnInitTimer(double async_id,
                                       double duration,
                                       bool single_shot) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  AddOp(async_id,
        AsyncOp::kTimer,
        NodeJson("timerScheduled",
                 std::pair{"duration", duration},
                 std::pair{"singleShot", single_shot}));
}

void AsyncDependencyGraph::OnInitMicrotask(double async_id) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  AddOp(async_id, AsyncOp::kMicrotask, NodeJson("enqueueMicrotask"));
}

void AsyncDependencyGraph::OnInitScript(double async_id) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  AddOp(async_id,
        AsyncOp::kScript,
        NodeJson("marker",
                 std::pair{"reason", std::string_view("ExecuteScript")},
                 std::pair{"name", std::string_view("StartExecution")}));
}

void AsyncDependencyGraph::OnDestroy(double async_id) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  ops_.erase(async_id);
}

void AsyncDependencyGraph::OnPush(double async_id, uint32_t offset) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  OnPop(offset);
  // Only grows: not-begun slots for JS pushes before setupDependencyGraph().
  windows_.resize(offset);
  auto it = ops_.find(async_id);
  if (it == ops_.end()) {
    windows_.push_back(std::nullopt);
    return;
  }
  windows_.push_back(
      DependencyExecution::Begin(RunNode(it->second.node, it->second.op)));
}

void AsyncDependencyGraph::OnPop(uint32_t length) {
  if (!v8::recordreplay::DependencyGraphEnabled() ||
      windows_.size() <= length) {
    return;
  }
  // The outermost: a window's depth is below its inner windows' unless
  // something unwound in between.
  std::optional<DependencyExecution> outermost;
  for (size_t i = length; i < windows_.size(); i++) {
    const std::optional<DependencyExecution>& window = windows_[i];
    if (window && (!outermost || window->depth() < outermost->depth())) {
      outermost = window;
    }
  }
  windows_.resize(length);
  if (outermost) outermost->End();
  DropEndedDispatches();
}

void AsyncDependencyGraph::OnBeginDispatch(Isolate* isolate,
                                           Local<Value> type) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  Local<Value> name =
      type->IsSymbol() ? type.As<Symbol>()->Description(isolate) : type;
  Utf8Value name_utf8(isolate, StringOrEmpty(isolate, name));
  PushDispatch(v8::recordreplay::NewDependencyGraphNode(
      NodeJson("emitEvent", std::pair{"type", name_utf8.ToStringView()})
          .c_str()));
}

int AsyncDependencyGraph::OnBeginRequest(Isolate* isolate,
                                         Local<Value> method,
                                         Local<Value> url) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return 0;
  Utf8Value method_utf8(isolate, StringOrEmpty(isolate, method));
  Utf8Value url_utf8(isolate, StringOrEmpty(isolate, url));
  int node = v8::recordreplay::NewDependencyGraphNode(
      NodeJson("requestStart",
               std::pair{"method", method_utf8.ToStringView()},
               std::pair{"url", url_utf8.ToStringView()})
          .c_str());
  PushDispatch(node);
  return node;
}

void AsyncDependencyGraph::OnBeginReceiveData(Isolate* isolate,
                                              Local<String> request_id,
                                              double length) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  Utf8Value id(isolate, request_id);
  PushDispatch(v8::recordreplay::NewDependencyGraphNode(
      NodeJson("networkReceiveData",
               std::pair{"requestId", id.ToStringView()},
               std::pair{"length", length})
          .c_str()));
}

void AsyncDependencyGraph::PushDispatch(int node) {
  dispatches_.push_back(DependencyExecution::Begin(node));
}

void AsyncDependencyGraph::OnEndDispatch() {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  // Dispatches whose own end a termination skipped, if an unwind ended them.
  DropEndedDispatches();
  if (dispatches_.empty()) return;
  std::optional<DependencyExecution> execution = dispatches_.back();
  dispatches_.pop_back();
  if (execution) execution->End();
}

void AsyncDependencyGraph::DropEndedDispatches() {
  while (!dispatches_.empty() && dispatches_.back() &&
         dispatches_.back()->ended()) {
    dispatches_.pop_back();
  }
}

int NewLockRequestDependencyNode(Isolate* isolate, Local<String> name) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return 0;
  Utf8Value name_utf8(isolate, name);
  return v8::recordreplay::NewDependencyGraphNode(
      NodeJson("lockManagerRequest",
               std::pair{"name", name_utf8.ToStringView()})
          .c_str());
}

int NewLockGrantedDependencyNode(int request_node) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return 0;
  int node = v8::recordreplay::NewDependencyGraphNode(
      R"({"kind":"lockRequestGranted"})");
  v8::recordreplay::AddDependencyGraphEdge(
      request_node, node, R"({"kind":"creator"})");
  return node;
}

void AsyncDependencyGraph::CreatePerIsolateProperties(
    IsolateData* isolate_data, Local<ObjectTemplate> target) {
  Isolate* isolate = isolate_data->isolate();
  SetMethod(isolate, target, "dependencyGraphInstrumented", Instrumented);
  SetMethod(isolate, target, "dependencyGraphInit", Init);
  SetMethod(isolate, target, "dependencyGraphInitTimer", InitTimer);
  SetMethod(isolate, target, "dependencyGraphInitMicrotask", InitMicrotask);
  SetMethod(isolate, target, "dependencyGraphDestroy", Destroy);
  SetMethod(isolate, target, "dependencyGraphPush", Push);
  SetMethod(isolate, target, "dependencyGraphPop", Pop);
  SetMethod(isolate, target, "dependencyGraphBeginDispatch", BeginDispatch);
  SetMethod(isolate, target, "dependencyGraphEndDispatch", EndDispatch);
  SetMethod(isolate, target, "dependencyGraphBeginRequest", BeginRequest);
  SetMethod(isolate, target, "dependencyGraphResponseStart", ResponseStart);
  SetMethod(isolate, target, "dependencyGraphResponseData", ResponseData);
  SetMethod(isolate, target, "dependencyGraphResponseEnd", ResponseEnd);
  SetMethod(isolate, target, "dependencyGraphNetworkRequest", NetworkRequest);
  SetMethod(
      isolate, target, "dependencyGraphBeginReceiveData", BeginReceiveData);
}

void AsyncDependencyGraph::RegisterExternalReferences(
    ExternalReferenceRegistry* registry) {
  registry->Register(Instrumented);
  registry->Register(Init);
  registry->Register(InitTimer);
  registry->Register(InitMicrotask);
  registry->Register(Destroy);
  registry->Register(Push);
  registry->Register(Pop);
  registry->Register(BeginDispatch);
  registry->Register(EndDispatch);
  registry->Register(BeginRequest);
  registry->Register(ResponseStart);
  registry->Register(ResponseData);
  registry->Register(ResponseEnd);
  registry->Register(NetworkRequest);
  registry->Register(BeginReceiveData);
}

}  // namespace node
