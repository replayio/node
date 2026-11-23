#include "node_dependency_graph.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include "env-inl.h"
#include "json_utils.h"
#include "node_external_reference.h"
#include "simdutf.h"
#include "util-inl.h"

namespace node {

using v8::Array;
using v8::ArrayBufferView;
using v8::Context;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::ObjectTemplate;
using v8::String;
using v8::Symbol;
using v8::Uint32;
using v8::Value;

namespace {

// A node's JSON: {"kind":kind, ...what write(writer) adds}.
template <typename Write>
std::string NodeJsonWith(std::string_view kind, Write write) {
  std::ostringstream out;
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  JSONWriter writer(out, true);
  writer.json_start();
  writer.json_keyvalue("kind", kind);
  write(writer);
  writer.json_end();
  return out.str();
}

// A node's JSON: {"kind":kind, ...fields}.
template <typename... Fields>
std::string NodeJson(std::string_view kind, const Fields&... fields) {
  return NodeJsonWith(kind, [&](JSONWriter& writer) {
    (writer.json_keyvalue(fields.first, fields.second), ...);
  });
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

// The array's own data element at index, if a string. Runs no JS: no getter,
// no prototype lookup.
std::optional<std::string> OwnStringElement(Isolate* isolate,
                                            Local<Context> context,
                                            Local<Array> array,
                                            uint32_t index) {
  Local<String> key;
  Local<Value> element;
  if (!array->HasRealIndexedProperty(context, index).FromMaybe(false) ||
      !Uint32::NewFromUnsigned(isolate, index)->ToString(context).ToLocal(
          &key) ||
      array->HasRealNamedCallbackProperty(context, key).FromMaybe(true) ||
      !array->Get(context, index).ToLocal(&element) || !element->IsString()) {
    return std::nullopt;
  }
  Utf8Value utf8(isolate, element);
  return std::string(utf8.ToStringView());
}

// The request headers requestStart reports, lowercase. Closed: no credentials
// (cookie, authorization).
constexpr std::array<std::string_view, 7> kRequestHeaderNames = {
    "host",
    "x-request-id",
    "traceparent",
    "content-type",
    "content-length",
    "content-encoding",
    "transfer-encoding",
};

struct RequestHeaders {
  // Per kRequestHeaderNames entry: its values in order.
  std::array<std::vector<std::string>, kRequestHeaderNames.size()> values;
  // Every name read, lowercase, also those not in kRequestHeaderNames. No
  // values.
  std::set<std::string> names;
  // The rawHeaders elements reached the parser's limit: elements past it, if
  // sent, were dropped. Reached, not passed: the parser keeps exactly that
  // many when it drops, so equal is also reported when none was dropped.
  bool capped = false;
  // rawHeaders, or a name element or a reported value element up to the
  // limit, was no own data string, so it was not read.
  bool unread = false;
};

// raw_headers: IncomingMessage#rawHeaders, [name, value, ...] elements.
// element_limit: the parser's maxHeaderPairs, which counts elements; not a
// positive number: none.
RequestHeaders ReadRequestHeaders(Isolate* isolate,
                                  Local<Value> raw_headers,
                                  Local<Value> element_limit) {
  RequestHeaders headers;
  if (!raw_headers->IsArray()) {
    headers.unread = true;
    return headers;
  }
  Local<Array> elements = raw_headers.As<Array>();
  Local<Context> context = isolate->GetCurrentContext();
  double limit =
      element_limit->IsNumber() ? element_limit.As<Number>()->Value() : 0;
  uint32_t count = elements->Length();
  if (limit > 0 && count >= limit) {
    headers.capped = true;
    count = static_cast<uint32_t>(limit);
  }
  for (uint32_t i = 0; i + 1 < count; i += 2) {
    // Frees each pair's handles: up to maxHeaderPairs elements are scanned.
    HandleScope scope(isolate);
    std::optional<std::string> name =
        OwnStringElement(isolate, context, elements, i);
    if (!name) {
      headers.unread = true;
      continue;
    }
    std::string lowercase = ToLower(*name);
    headers.names.insert(lowercase);
    auto it = std::find(kRequestHeaderNames.begin(),
                        kRequestHeaderNames.end(),
                        std::string_view(lowercase));
    if (it == kRequestHeaderNames.end()) continue;
    std::optional<std::string> value =
        OwnStringElement(isolate, context, elements, i + 1);
    if (!value) {
      headers.unread = true;
      continue;
    }
    headers.values[it - kRequestHeaderNames.begin()].push_back(
        std::move(*value));
  }
  return headers;
}

std::string RequestStartJson(std::string_view method,
                             std::string_view url,
                             const RequestHeaders& headers) {
  return NodeJsonWith("requestStart", [&](JSONWriter& writer) {
    writer.json_keyvalue("method", method);
    writer.json_keyvalue("url", url);
    writer.json_objectstart("headers");
    for (size_t i = 0; i < kRequestHeaderNames.size(); i++) {
      if (headers.values[i].empty()) continue;
      writer.json_arraystart(kRequestHeaderNames[i]);
      for (const std::string& value : headers.values[i]) {
        writer.json_element(std::string_view(value));
      }
      writer.json_arrayend();
    }
    writer.json_objectend();
    writer.json_arraystart("headerNames");
    for (const std::string& name : headers.names) {
      writer.json_element(std::string_view(name));
    }
    writer.json_arrayend();
    writer.json_keyvalue("headersCapped", headers.capped);
    writer.json_keyvalue("headersUnread", headers.unread);
  });
}

// requestEnd reports the bytes of a body of up to this many; of a longer one,
// none.
constexpr uint64_t kMaxRequestBodyBytes = 64 * 1024;

// The most body bytes of a process that requestEnd nodes report, together with
// those kept for messages not complete yet. A body past it is dropped. In the
// graph JSON, base64 makes them 4/3 as many. First come, first kept: once it
// is spent, every later body of the process is dropped, also those of the
// requests a user cares about. Accepted for a bounded graph; requestEnd says
// so (bodyDropped "budget").
constexpr uint64_t kRequestBodyBudgetBytes = 4 * 1024 * 1024;

std::string_view RequestBodyDropName(RequestBodyDrop drop) {
  switch (drop) {
    case RequestBodyDrop::kTooLong:
      return "too-long";
    case RequestBodyDrop::kBudget:
      return "budget";
  }
  UNREACHABLE();
}

std::string_view DependencyGraphFeatureName(DependencyGraphFeature feature) {
  switch (feature) {
    case DependencyGraphFeature::kRequestStart:
      return "requestStart";
    case DependencyGraphFeature::kNetworkRequest:
      return "networkRequest";
    case DependencyGraphFeature::kNetworkReceiveData:
      return "networkReceiveData";
    case DependencyGraphFeature::kRequestHeaders:
      return "requestHeaders";
    case DependencyGraphFeature::kRequestBody:
      return "requestBody";
    case DependencyGraphFeature::kRequestHeaderNames:
      return "requestHeaderNames";
    case DependencyGraphFeature::kRequestParts:
      return "requestParts";
  }
  UNREACHABLE();
}

// This build's features: every DependencyGraphFeature.
constexpr DependencyGraphFeature kDependencyGraphFeatures[] = {
    DependencyGraphFeature::kRequestStart,
    DependencyGraphFeature::kNetworkRequest,
    DependencyGraphFeature::kNetworkReceiveData,
    DependencyGraphFeature::kRequestHeaders,
    DependencyGraphFeature::kRequestBody,
    DependencyGraphFeature::kRequestHeaderNames,
    DependencyGraphFeature::kRequestParts,
};

std::string Base64(const std::string& bytes) {
  std::string base64(simdutf::base64_length_from_binary(bytes.size()), '\0');
  base64.resize(
      simdutf::binary_to_base64(bytes.data(), bytes.size(), base64.data()));
  return base64;
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
      args.GetIsolate(), args[0], args[1], args[2], args[3]));
}

void BeginReceiveData(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString());
  CHECK(args[2]->IsString());
  CHECK(args[3]->IsString());
  GetCurrent(args).OnBeginReceiveData(args.GetIsolate(),
                                      args[0].As<String>(),
                                      NumberArg(args, 1),
                                      args[2].As<String>(),
                                      args[3].As<String>());
}

int RequestArg(const FunctionCallbackInfo<Value>& args, int index) {
  CHECK(args[index]->IsInt32());
  return args[index].As<v8::Int32>()->Value();
}

// A node with a request edge from its request's node (0: none).
int NewRequestPartNode(int request, const std::string& json) {
  int node = v8::recordreplay::NewDependencyGraphNode(json.c_str());
  v8::recordreplay::AddDependencyGraphEdge(
      request, node, R"({"kind":"request"})");
  return node;
}

// The body of a server request: AsyncDependencyGraph::OnRequestBodyBytes,
// OnBeginRequestData, OnBeginRequestEnd, OnRequestAbandoned.
void RequestBodyBytes(const FunctionCallbackInfo<Value>& args) {
  int request = RequestArg(args, 0);
  CHECK(args[1]->IsArrayBufferView());
  GetCurrent(args).OnRequestBodyBytes(request, args[1].As<ArrayBufferView>());
}

void BeginRequestData(const FunctionCallbackInfo<Value>& args) {
  int request = RequestArg(args, 0);
  CHECK(args[1]->IsArrayBufferView());
  GetCurrent(args).OnBeginRequestData(
      request, args[1].As<ArrayBufferView>()->ByteLength());
}

void BeginRequestEnd(const FunctionCallbackInfo<Value>& args) {
  GetCurrent(args).OnBeginRequestEnd(RequestArg(args, 0));
}

void RequestAbandoned(const FunctionCallbackInfo<Value>& args) {
  GetCurrent(args).OnRequestAbandoned(RequestArg(args, 0));
}

// A server response (http.ServerResponse): responseStart{status} at its head,
// responseData{length} per body write and responseEnd at its end. Each is made
// in the JS that writes it, which is its cause.
void ResponseStart(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsInt32());
  int request = RequestArg(args, 1);
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  NewRequestPartNode(
      request,
      NodeJson("responseStart",
               std::pair{"status", args[0].As<v8::Int32>()->Value()}));
}

void ResponseData(const FunctionCallbackInfo<Value>& args) {
  double length = NumberArg(args, 0);
  int request = RequestArg(args, 1);
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  NewRequestPartNode(request,
                     NodeJson("responseData", std::pair{"length", length}));
}

void ResponseEnd(const FunctionCallbackInfo<Value>& args) {
  int request = RequestArg(args, 0);
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  NewRequestPartNode(request, R"({"kind":"responseEnd"})");
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
        NodeJsonWith("marker", [](JSONWriter& writer) {
          writer.json_keyvalue("reason", std::string_view("ExecuteScript"));
          writer.json_keyvalue("name", std::string_view("StartExecution"));
          writer.json_arraystart("features");
          for (DependencyGraphFeature feature : kDependencyGraphFeatures) {
            writer.json_element(DependencyGraphFeatureName(feature));
          }
          writer.json_arrayend();
        }));
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
                                         Local<Value> url,
                                         Local<Value> raw_headers,
                                         Local<Value> header_element_limit) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return 0;
  Utf8Value method_utf8(isolate, StringOrEmpty(isolate, method));
  Utf8Value url_utf8(isolate, StringOrEmpty(isolate, url));
  int node = v8::recordreplay::NewDependencyGraphNode(
      RequestStartJson(
          method_utf8.ToStringView(),
          url_utf8.ToStringView(),
          ReadRequestHeaders(isolate, raw_headers, header_element_limit))
          .c_str());
  PushDispatch(node);
  return node;
}

void AsyncDependencyGraph::OnRequestBodyBytes(int request,
                                              Local<ArrayBufferView> chunk) {
  if (!v8::recordreplay::DependencyGraphEnabled() || !request) return;
  RequestBody& body = request_bodies_[request];
  size_t length = chunk->ByteLength();
  body.length += length;
  if (body.dropped) return;
  if (body.length > kMaxRequestBodyBytes ||
      body_bytes_ + length > kRequestBodyBudgetBytes) {
    DropBody(body);
  } else {
    size_t offset = body.bytes.size();
    body.bytes.resize(offset + length);
    chunk->CopyContents(body.bytes.data() + offset, length);
    body_bytes_ += length;
  }
}

void AsyncDependencyGraph::DropBody(RequestBody& body) {
  body_bytes_ -= body.bytes.size();
  std::string().swap(body.bytes);
  body.dropped = true;
}

void AsyncDependencyGraph::OnBeginRequestData(int request, size_t length) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  PushDispatch(request ? NewRequestPartNode(
                             request,
                             NodeJson("requestData",
                                      std::pair{"length", uint64_t{length}}))
                       : 0);
}

void AsyncDependencyGraph::OnBeginRequestEnd(int request) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  PushDispatch(request ? NewRequestEndNode(request) : 0);
}

int AsyncDependencyGraph::NewRequestEndNode(int request) {
  RequestBody body;
  if (auto it = request_bodies_.find(request); it != request_bodies_.end()) {
    body = std::move(it->second);
    request_bodies_.erase(it);
  }
  // From the length alone, so too-long wins over budget, whichever dropped the
  // bytes first: more budget would not have kept a too-long body.
  std::optional<RequestBodyDrop> drop;
  if (body.length > kMaxRequestBodyBytes) {
    drop = RequestBodyDrop::kTooLong;
  } else if (body.dropped) {
    drop = RequestBodyDrop::kBudget;
  }
  // The bytes stay in body_bytes_: the node reports them.
  return NewRequestPartNode(
      request, NodeJsonWith("requestEnd", [&](JSONWriter& writer) {
        writer.json_keyvalue("length", body.length);
        if (drop) {
          writer.json_keyvalue("bodyDropped", RequestBodyDropName(*drop));
        } else {
          writer.json_keyvalue("body", std::string_view(Base64(body.bytes)));
        }
      }));
}

void AsyncDependencyGraph::OnRequestAbandoned(int request) {
  if (!v8::recordreplay::DependencyGraphEnabled() || !request) return;
  auto it = request_bodies_.find(request);
  if (it == request_bodies_.end()) return;
  body_bytes_ -= it->second.bytes.size();
  request_bodies_.erase(it);
}

void AsyncDependencyGraph::OnBeginReceiveData(Isolate* isolate,
                                              Local<String> request_id,
                                              double length,
                                              Local<String> part,
                                              Local<String> timed_by) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  Utf8Value id(isolate, request_id);
  Utf8Value part_name(isolate, part);
  Utf8Value timed_by_name(isolate, timed_by);
  PushDispatch(v8::recordreplay::NewDependencyGraphNode(
      NodeJson("networkReceiveData",
               std::pair{"requestId", id.ToStringView()},
               std::pair{"length", length},
               std::pair{"part", part_name.ToStringView()},
               std::pair{"timedBy", timed_by_name.ToStringView()})
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
  SetMethod(
      isolate, target, "dependencyGraphRequestBodyBytes", RequestBodyBytes);
  SetMethod(
      isolate, target, "dependencyGraphBeginRequestData", BeginRequestData);
  SetMethod(
      isolate, target, "dependencyGraphBeginRequestEnd", BeginRequestEnd);
  SetMethod(
      isolate, target, "dependencyGraphRequestAbandoned", RequestAbandoned);
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
  registry->Register(RequestBodyBytes);
  registry->Register(BeginRequestData);
  registry->Register(BeginRequestEnd);
  registry->Register(RequestAbandoned);
  registry->Register(ResponseStart);
  registry->Register(ResponseData);
  registry->Register(ResponseEnd);
  registry->Register(NetworkRequest);
  registry->Register(BeginReceiveData);
}

}  // namespace node
