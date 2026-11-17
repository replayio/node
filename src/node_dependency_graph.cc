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
  Utf8Value name_utf8(isolate,
                      name->IsString() ? name : String::Empty(isolate));
  dispatches_.push_back(DependencyExecution::Begin(
      v8::recordreplay::NewDependencyGraphNode(
          NodeJson("emitEvent", std::pair{"type", name_utf8.ToStringView()})
              .c_str())));
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
}

}  // namespace node
