#include "node_dependency_graph.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

#include "env-inl.h"
#include "json_utils.h"
#include "node_external_reference.h"
#include "path.h"
#include "req_wrap-inl.h"
#include "simdutf.h"
#include "util-inl.h"
#include "uv.h"

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

// wire_ids: the array of the x-replay-wire-id values received, own strings
// (lib/internal/recordreplay/dependency_graph_http.js takeWireIds).
std::vector<std::string> ReadWireIds(Isolate* isolate, Local<Value> wire_ids) {
  CHECK(wire_ids->IsArray());
  Local<Array> elements = wire_ids.As<Array>();
  Local<Context> context = isolate->GetCurrentContext();
  std::vector<std::string> ids;
  for (uint32_t i = 0; i < elements->Length(); i++) {
    HandleScope scope(isolate);
    std::optional<std::string> id =
        OwnStringElement(isolate, context, elements, i);
    CHECK(id.has_value());
    ids.push_back(std::move(*id));
  }
  return ids;
}

std::string RequestStartJson(std::string_view method,
                             std::string_view url,
                             const RequestHeaders& headers,
                             const std::vector<std::string>& wire_ids) {
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
    writer.json_arraystart("wireIds");
    for (const std::string& id : wire_ids) {
      writer.json_element(std::string_view(id));
    }
    writer.json_arrayend();
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
    case DependencyGraphFeature::kFsOps:
      return "fsOps";
    case DependencyGraphFeature::kReactionJobs:
      return "reactionJobs";
    case DependencyGraphFeature::kCount:
      break;
  }
  UNREACHABLE();
}

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
      args.GetIsolate(), args[0], args[1], args[2], args[3], args[4]));
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

#ifndef _WIN32

// A path an fs request was given. No syscall resolves it: replaying can't
// issue one, so symlinks stay unresolved.
struct FsPath {
  // Lexically normalized, against the cwd if relative. nullopt: unknown, also
  // for a path with a .. component: what precedes it may be a symlink.
  std::optional<std::string> absolute;
  // As given, if relative or absolute is unknown.
  std::optional<std::string> raw;
};

bool HasParentComponent(std::string_view path) {
  return ("/" + std::string(path) + "/").find("/../") != std::string::npos;
}

FsPath ResolveFsPath(std::string_view path,
                     const std::optional<std::string>& cwd) {
  const bool relative = !path.starts_with('/');
  FsPath resolved;
  if (!path.empty() && !HasParentComponent(path) && (!relative || cwd)) {
    const std::string joined =
        relative ? *cwd + "/" + std::string(path) : std::string(path);
    resolved.absolute = "/" + NormalizeString(joined, false, "/");
  }
  if (relative || !resolved.absolute) resolved.raw = path;
  return resolved;
}

// What an fs request or an FsJob reports when issued. A request's is read
// then: its buffers and offset change while it runs.
struct FsIssue {
  std::variant<uv_fs_type, FsJob> op;
  bool sync;
  std::optional<FsPath> path;
  std::optional<FsPath> new_path;
  // symlink: the link's contents, which resolve against the link's directory,
  // not the cwd.
  std::optional<std::string> target;
  std::optional<uv_file> fd;
  // The path of the open that returned fd, if seen.
  std::optional<std::string> fd_path;
  // open.
  std::optional<int> flags;
  // Explicit only: none for the current position.
  std::optional<int64_t> offset;
  std::optional<uint64_t> length;
};

struct PendingFsOp {
  FsIssue issue;
  // Async: its fsOp node (0: none). Sync: 0, it is added at completion.
  int node;
};

#endif  // _WIN32

// fs ops (InstallFsDependencyGraph). Main thread only.
struct FsOps {
  // Whether fsOp nodes are reported: the observers are installed.
  bool reported = false;
  std::optional<std::string> cwd;
  // The recorder's own I/O scopes entered and not left
  // (dependencyGraphBeginRecorderIO): their requests are not the program's.
  uint32_t recorder_io_depth = 0;
  // The fsDone node whose callback is running and began no window yet, 0 if
  // none.
  int completion = 0;
#ifndef _WIN32
  // Per request posted, until its completion.
  std::unordered_map<const uv_fs_t*, PendingFsOp> pending;
  // Per fd an open's completion returned, until its close is posted or it is
  // handed over (OnFsFdHandedOver): the open's path.
  std::unordered_map<uv_file, std::string> fd_paths;
#endif  // _WIN32
};

FsOps& GetFsOps() {
  // Never destroyed: requests complete until exit.
  static FsOps* ops = new FsOps();
  return *ops;
}

#ifndef _WIN32

// Not while events are disallowed: GC closes fds at points that differ between
// replays, and node ids taken there would too.
bool FsOpsReportable() {
  return v8::recordreplay::DependencyGraphEnabled() &&
         !v8::recordreplay::AreEventsDisallowed("FsDependencyGraph");
}

std::string_view FsOpName(uv_fs_type type) {
  switch (type) {
    case UV_FS_UNKNOWN:
      return "unknown";
    case UV_FS_CUSTOM:
      return "custom";
    case UV_FS_OPEN:
      return "open";
    case UV_FS_CLOSE:
      return "close";
    case UV_FS_READ:
      return "read";
    case UV_FS_WRITE:
      return "write";
    case UV_FS_SENDFILE:
      return "sendfile";
    case UV_FS_STAT:
      return "stat";
    case UV_FS_LSTAT:
      return "lstat";
    case UV_FS_FSTAT:
      return "fstat";
    case UV_FS_FTRUNCATE:
      return "ftruncate";
    case UV_FS_UTIME:
      return "utime";
    case UV_FS_FUTIME:
      return "futime";
    case UV_FS_ACCESS:
      return "access";
    case UV_FS_CHMOD:
      return "chmod";
    case UV_FS_FCHMOD:
      return "fchmod";
    case UV_FS_FSYNC:
      return "fsync";
    case UV_FS_FDATASYNC:
      return "fdatasync";
    case UV_FS_UNLINK:
      return "unlink";
    case UV_FS_RMDIR:
      return "rmdir";
    case UV_FS_MKDIR:
      return "mkdir";
    case UV_FS_MKDTEMP:
      return "mkdtemp";
    case UV_FS_RENAME:
      return "rename";
    case UV_FS_SCANDIR:
      return "scandir";
    case UV_FS_LINK:
      return "link";
    case UV_FS_SYMLINK:
      return "symlink";
    case UV_FS_READLINK:
      return "readlink";
    case UV_FS_CHOWN:
      return "chown";
    case UV_FS_FCHOWN:
      return "fchown";
    case UV_FS_REALPATH:
      return "realpath";
    case UV_FS_COPYFILE:
      return "copyfile";
    case UV_FS_LCHOWN:
      return "lchown";
    case UV_FS_OPENDIR:
      return "opendir";
    case UV_FS_READDIR:
      return "readdir";
    case UV_FS_CLOSEDIR:
      return "closedir";
    case UV_FS_STATFS:
      return "statfs";
    case UV_FS_MKSTEMP:
      return "mkstemp";
    case UV_FS_LUTIME:
      return "lutime";
  }
  UNREACHABLE();
}

std::string_view FsOpName(FsJob job) {
  switch (job) {
    case FsJob::kReadFile:
      return "readFile";
    case FsJob::kWriteFile:
      return "writeFile";
    case FsJob::kReadDirRecursive:
      return "readdirRecursive";
    case FsJob::kCpCheckPaths:
      return "cpCheckPaths";
    case FsJob::kCpFile:
      return "cpFile";
    case FsJob::kCpDir:
      return "cpDir";
    case FsJob::kRm:
      return "rm";
    case FsJob::kChdir:
      return "chdir";
  }
  UNREACHABLE();
}

bool FsOpTakesFd(uv_fs_type type) {
  switch (type) {
    case UV_FS_CLOSE:
    case UV_FS_READ:
    case UV_FS_WRITE:
    case UV_FS_SENDFILE:
    case UV_FS_FSTAT:
    case UV_FS_FTRUNCATE:
    case UV_FS_FUTIME:
    case UV_FS_FCHMOD:
    case UV_FS_FSYNC:
    case UV_FS_FDATASYNC:
    case UV_FS_FCHOWN:
      return true;
    default:
      return false;
  }
}

// The path of these is a template until their completion.
bool FsOpCreatesPath(uv_fs_type type) {
  return type == UV_FS_MKDTEMP || type == UV_FS_MKSTEMP;
}

FsIssue ReadFsIssue(const uv_fs_t* req, const FsOps& ops) {
  FsIssue issue{req->fs_type, req->cb == nullptr};
  if (req->path != nullptr) {
    if (req->fs_type == UV_FS_SYMLINK) {
      issue.target = req->path;
    } else if (FsOpCreatesPath(req->fs_type)) {
      issue.path = FsPath{std::nullopt, req->path};
    } else {
      issue.path = ResolveFsPath(req->path, ops.cwd);
    }
  }
  if (req->new_path != nullptr) {
    issue.new_path = ResolveFsPath(req->new_path, ops.cwd);
  }
  if (FsOpTakesFd(req->fs_type)) {
    issue.fd = req->file;
    if (auto it = ops.fd_paths.find(req->file); it != ops.fd_paths.end()) {
      issue.fd_path = it->second;
    }
  }
  switch (req->fs_type) {
    case UV_FS_OPEN:
      issue.flags = req->flags;
      break;
    case UV_FS_READ:
    case UV_FS_WRITE: {
      if (req->off >= 0) issue.offset = req->off;
      uint64_t length = 0;
      for (unsigned int i = 0; i < req->nbufs; i++) length += req->bufs[i].len;
      issue.length = length;
      break;
    }
    case UV_FS_FTRUNCATE:
      issue.offset = req->off;
      break;
    default:
      break;
  }
  return issue;
}

// Bits of open flags by name, a combined one (O_SYNC has O_DSYNC's bit) before
// its parts.
constexpr std::pair<int, std::string_view> kOpenFlagNames[] = {
    {O_CREAT, "O_CREAT"},
    {O_EXCL, "O_EXCL"},
    {O_NOCTTY, "O_NOCTTY"},
    {O_TRUNC, "O_TRUNC"},
    {O_APPEND, "O_APPEND"},
    {O_NONBLOCK, "O_NONBLOCK"},
    {O_SYNC, "O_SYNC"},
    {O_DSYNC, "O_DSYNC"},
    {O_DIRECTORY, "O_DIRECTORY"},
    {O_NOFOLLOW, "O_NOFOLLOW"},
    {O_CLOEXEC, "O_CLOEXEC"},
#ifdef O_NOATIME
    {O_NOATIME, "O_NOATIME"},
#endif
#ifdef O_DIRECT
    {O_DIRECT, "O_DIRECT"},
#endif
};

// flags: the access mode's and each known bit's name; otherFlags: the bits
// left, if any.
void WriteOpenFlags(JSONWriter& writer, int flags) {
  writer.json_arraystart("flags");
  switch (flags & O_ACCMODE) {
    case O_RDONLY:
      writer.json_element(std::string_view("O_RDONLY"));
      break;
    case O_WRONLY:
      writer.json_element(std::string_view("O_WRONLY"));
      break;
    case O_RDWR:
      writer.json_element(std::string_view("O_RDWR"));
      break;
  }
  int other = flags & ~O_ACCMODE;
  for (const auto& [bits, name] : kOpenFlagNames) {
    if ((other & bits) == bits) {
      writer.json_element(name);
      other &= ~bits;
    }
  }
  writer.json_arrayend();
  if (other) writer.json_keyvalue("otherFlags", other);
}

// path and rawPath, or newPath and rawNewPath.
void WriteFsPath(JSONWriter& writer,
                 const FsPath& path,
                 std::string_view absolute_key,
                 std::string_view raw_key) {
  if (path.absolute) {
    writer.json_keyvalue(absolute_key, std::string_view(*path.absolute));
  }
  if (path.raw) writer.json_keyvalue(raw_key, std::string_view(*path.raw));
}

void WriteFsIssue(JSONWriter& writer, const FsIssue& issue) {
  writer.json_keyvalue(
      "op", std::visit([](auto op) { return FsOpName(op); }, issue.op));
  writer.json_keyvalue("sync", issue.sync);
  if (issue.path) WriteFsPath(writer, *issue.path, "path", "rawPath");
  if (issue.new_path) {
    WriteFsPath(writer, *issue.new_path, "newPath", "rawNewPath");
  }
  if (issue.target) {
    writer.json_keyvalue("target", std::string_view(*issue.target));
  }
  if (issue.fd) writer.json_keyvalue("fd", *issue.fd);
  if (issue.fd_path) {
    writer.json_keyvalue("fdPath", std::string_view(*issue.fd_path));
  }
  if (issue.flags) WriteOpenFlags(writer, *issue.flags);
  if (issue.offset) writer.json_keyvalue("offset", *issue.offset);
  if (issue.length) writer.json_keyvalue("length", *issue.length);
}

std::string_view FileTypeName(uint64_t mode) {
  switch (mode & S_IFMT) {
    case S_IFREG:
      return "file";
    case S_IFDIR:
      return "directory";
    case S_IFLNK:
      return "symlink";
    case S_IFIFO:
      return "fifo";
    case S_IFSOCK:
      return "socket";
    case S_IFCHR:
      return "characterDevice";
    case S_IFBLK:
      return "blockDevice";
  }
  return "unknown";
}

// result (fd, bytes, entries, 0) or error (the uv error name); a stat's
// result; the path mkdtemp and mkstemp created, unless relative: the cwd it
// resolved against may have changed.
void WriteFsResult(JSONWriter& writer, const uv_fs_t* req) {
  if (req->result < 0) {
    writer.json_keyvalue(
        "error",
        std::string_view(uv_err_name(static_cast<int>(req->result))));
    return;
  }
  writer.json_keyvalue("result", int64_t{req->result});
  if (FsOpCreatesPath(req->fs_type)) {
    const FsPath created = ResolveFsPath(req->path, std::nullopt);
    if (created.absolute) {
      writer.json_keyvalue("path", std::string_view(*created.absolute));
    }
  }
  if (req->fs_type != UV_FS_STAT && req->fs_type != UV_FS_LSTAT &&
      req->fs_type != UV_FS_FSTAT) {
    return;
  }
  const uv_stat_t& stat = req->statbuf;
  writer.json_keyvalue("size", stat.st_size);
  writer.json_keyvalue("modifiedTime",
                       stat.st_mtim.tv_sec * 1e3 + stat.st_mtim.tv_nsec / 1e6);
  writer.json_keyvalue("inode", stat.st_ino);
  writer.json_keyvalue("device", stat.st_dev);
  writer.json_keyvalue("fileType", FileTypeName(stat.st_mode));
}

// The asyncInit node of the op that posted the request, 0 if none: node posts
// its async requests from a ReqWrap, which points req->data at itself
// (ReqWrap::Dispatched). Requests posted otherwise, e.g. by addons, don't.
int FsRequestInitNode(uv_fs_t* req) {
  ReqWrap<uv_fs_t>* wrap = ReqWrap<uv_fs_t>::from_req(req);
  if (req->data != wrap) return 0;
  return wrap->env()->async_hooks()->dependency_graph().InitNode(
      wrap->get_async_id());
}

void OnFsIssue(uv_fs_t* req) {
  if (!FsOpsReportable()) return;
  FsOps& ops = GetFsOps();
  if (ops.recorder_io_depth) return;
  PendingFsOp pending{ReadFsIssue(req, ops), 0};
  if (!pending.issue.sync) {
    pending.node = v8::recordreplay::NewDependencyGraphNode(
        NodeJsonWith("fsOp",
                     [&](JSONWriter& writer) {
                       WriteFsIssue(writer, pending.issue);
                     })
            .c_str());
    v8::recordreplay::AddDependencyGraphEdge(
        FsRequestInitNode(req), pending.node, R"({"kind":"fsOperation"})");
  }
  if (req->fs_type == UV_FS_CLOSE) ops.fd_paths.erase(req->file);
  ops.pending.insert_or_assign(req, std::move(pending));
}

// An unknown path forgets that of an earlier open.
void SetFdPath(FsOps& ops, uv_file fd, const std::optional<FsPath>& path) {
  if (path && path->absolute) {
    ops.fd_paths.insert_or_assign(fd, *path->absolute);
  } else {
    ops.fd_paths.erase(fd);
  }
}

void OnFsDone(uv_fs_t* req) {
  if (!FsOpsReportable()) return;
  FsOps& ops = GetFsOps();
  auto it = ops.pending.find(req);
  if (it == ops.pending.end()) return;
  PendingFsOp pending = std::move(it->second);
  ops.pending.erase(it);
  if (pending.issue.sync) {
    v8::recordreplay::NewDependencyGraphNode(
        NodeJsonWith("fsOp", [&](JSONWriter& writer) {
          WriteFsIssue(writer, pending.issue);
          WriteFsResult(writer, req);
        }).c_str());
  } else if (pending.node) {
    int node = v8::recordreplay::NewDependencyGraphNode(
        NodeJsonWith("fsDone", [&](JSONWriter& writer) {
          WriteFsResult(writer, req);
        }).c_str());
    v8::recordreplay::AddDependencyGraphEdge(
        pending.node, node, R"({"kind":"fsCompletion"})");
    ops.completion = node;
  }
  if (req->fs_type == UV_FS_OPEN && req->result >= 0) {
    SetFdPath(ops, static_cast<uv_file>(req->result), pending.issue.path);
  }
}

void OnFsDispatched() {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  GetFsOps().completion = 0;
}

bool FsJobsReported() {
  if (!FsOpsReportable()) return false;
  const FsOps& ops = GetFsOps();
  return ops.reported && !ops.recorder_io_depth;
}

FsIssue ReadFsJobIssue(const FsJobIssue& job, bool sync, const FsOps& ops) {
  FsIssue issue{job.job, sync};
  issue.path = ResolveFsPath(job.path, ops.cwd);
  if (job.new_path) issue.new_path = ResolveFsPath(*job.new_path, ops.cwd);
  issue.flags = job.flags;
  issue.length = job.length;
  return issue;
}

void WriteFsJobResult(JSONWriter& writer, const FsJobResult& result) {
  if (result.error) {
    writer.json_keyvalue("error", std::string_view(uv_err_name(result.error)));
    if (result.error_op) {
      writer.json_keyvalue("errorOp", std::string_view(result.error_op));
    }
  }
  if (result.failed) writer.json_keyvalue("failed", true);
  if (result.result) writer.json_keyvalue("result", *result.result);
  if (result.fd) writer.json_keyvalue("fd", *result.fd);
}

#endif  // _WIN32

// The recorder's own I/O (lib/internal/recordreplay) runs between these: its
// fs requests add no fsOp nodes. Scopes nest.
void BeginRecorderIO(const FunctionCallbackInfo<Value>& args) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  GetFsOps().recorder_io_depth++;
}

void EndRecorderIO(const FunctionCallbackInfo<Value>& args) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return;
  FsOps& ops = GetFsOps();
  CHECK_GT(ops.recorder_io_depth, 0);
  ops.recorder_io_depth--;
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
          // This build's features: every DependencyGraphFeature, kFsOps if
          // reported.
          const FsOps& fs_ops = GetFsOps();
          writer.json_arraystart("features");
          for (int i = 0; i < static_cast<int>(DependencyGraphFeature::kCount);
               i++) {
            auto feature = static_cast<DependencyGraphFeature>(i);
            if (feature == DependencyGraphFeature::kFsOps && !fs_ops.reported) {
              continue;
            }
            writer.json_element(DependencyGraphFeatureName(feature));
          }
          writer.json_arrayend();
          if (fs_ops.reported && fs_ops.cwd) {
            writer.json_keyvalue("cwd", std::string_view(*fs_ops.cwd));
          }
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
  // The completion dispatches the first window that begins in its callback.
  const int fs_done = std::exchange(GetFsOps().completion, 0);
  auto it = ops_.find(async_id);
  if (it == ops_.end()) {
    windows_.push_back(std::nullopt);
    return;
  }
  const int run = RunNode(it->second.node, it->second.op);
  if (it->second.op == AsyncOp::kGeneric) {
    v8::recordreplay::AddDependencyGraphEdge(
        fs_done, run, R"({"kind":"fsCallback"})");
  }
  windows_.push_back(DependencyExecution::Begin(run));
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
                                         Local<Value> header_element_limit,
                                         Local<Value> wire_ids) {
  if (!v8::recordreplay::DependencyGraphEnabled()) return 0;
  Utf8Value method_utf8(isolate, StringOrEmpty(isolate, method));
  Utf8Value url_utf8(isolate, StringOrEmpty(isolate, url));
  int node = v8::recordreplay::NewDependencyGraphNode(
      RequestStartJson(
          method_utf8.ToStringView(),
          url_utf8.ToStringView(),
          ReadRequestHeaders(isolate, raw_headers, header_element_limit),
          ReadWireIds(isolate, wire_ids))
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

int AsyncDependencyGraph::InitNode(double async_id) const {
  auto it = ops_.find(async_id);
  return it == ops_.end() ? 0 : it->second.node;
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

void InstallFsDependencyGraph() {
#ifndef _WIN32
  if (!v8::recordreplay::DependencyGraphEnabled() ||
      !v8::recordreplay::FeatureEnabled("dependency-graph-fs")) {
    return;
  }
  GetFsOps().reported = true;
  uv_fs_set_observers(OnFsIssue, OnFsDone, OnFsDispatched);
#endif  // _WIN32
}

void OnFsFdHandedOver(int fd) {
#ifndef _WIN32
  if (!FsOpsReportable()) return;
  GetFsOps().fd_paths.erase(fd);
#endif  // _WIN32
}

int OnFsJobIssue(AsyncWrap* wrap, const FsJobIssue& job) {
#ifndef _WIN32
  if (!FsJobsReported()) return 0;
  FsIssue issue = ReadFsJobIssue(job, false, GetFsOps());
  int node = v8::recordreplay::NewDependencyGraphNode(
      NodeJsonWith("fsOp", [&](JSONWriter& writer) {
        WriteFsIssue(writer, issue);
      }).c_str());
  v8::recordreplay::AddDependencyGraphEdge(
      wrap->env()->async_hooks()->dependency_graph().InitNode(
          wrap->get_async_id()),
      node,
      R"({"kind":"fsOperation"})");
  return node;
#else
  return 0;
#endif  // _WIN32
}

FsJobDoneScope::FsJobDoneScope(int node,
                               const FsJobIssue& job,
                               const FsJobResult& result) {
#ifndef _WIN32
  if (!node || !FsJobsReported()) return;
  FsOps& ops = GetFsOps();
  int done = v8::recordreplay::NewDependencyGraphNode(
      NodeJsonWith("fsDone", [&](JSONWriter& writer) {
        WriteFsJobResult(writer, result);
      }).c_str());
  v8::recordreplay::AddDependencyGraphEdge(
      node, done, R"({"kind":"fsCompletion"})");
  ops.completion = done;
  // Not of a relative path: the cwd may have changed since the open.
  if (result.fd) {
    SetFdPath(ops, *result.fd, ResolveFsPath(job.path, std::nullopt));
  }
#endif  // _WIN32
}

FsJobDoneScope::~FsJobDoneScope() {
#ifndef _WIN32
  OnFsDispatched();
#endif  // _WIN32
}

void OnFsJobSync(const FsJobIssue& job, const FsJobResult& result) {
#ifndef _WIN32
  if (!FsJobsReported()) return;
  FsIssue issue = ReadFsJobIssue(job, true, GetFsOps());
  v8::recordreplay::NewDependencyGraphNode(
      NodeJsonWith("fsOp", [&](JSONWriter& writer) {
        WriteFsIssue(writer, issue);
        WriteFsJobResult(writer, result);
      }).c_str());
#endif  // _WIN32
}

void RecordFsDependencyGraphCwd() {
  if (!v8::recordreplay::DependencyGraphInstrumented()) return;
  // Read when recording too: a getcwd only when replaying would be a syscall
  // the recording doesn't have.
  char cwd[PATH_MAX_BYTES];
  size_t size = sizeof(cwd);
  size_t length = v8::recordreplay::RecordReplayValue(
      "FsDependencyGraphCwdLength", uv_cwd(cwd, &size) == 0 ? size : 0);
  CHECK_LE(length, sizeof(cwd));
  std::optional<std::string>& known = GetFsOps().cwd;
  known.reset();
  if (!length) return;
  v8::recordreplay::RecordReplayBytes("FsDependencyGraphCwd", cwd, length);
  known = std::string(cwd, length);
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
  SetMethod(
      isolate, target, "dependencyGraphBeginRecorderIO", BeginRecorderIO);
  SetMethod(isolate, target, "dependencyGraphEndRecorderIO", EndRecorderIO);
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
  registry->Register(BeginRecorderIO);
  registry->Register(EndRecorderIO);
}

}  // namespace node
