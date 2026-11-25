// Replay's node integration: loading the record/replay driver at startup and
// the process binding's record/replay methods.

#include "env-inl.h"
#include "inspector_agent.h"
#include "node.h"
#include "node_external_reference.h"
#include "node_internals.h"
#include "replayio.h"
#include "util-inl.h"
#include "v8-inspector.h"
#include "v8.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* AnnotationHookJSName = "recordreplay.annotationHook";

namespace v8 {

extern void FunctionCallbackIsRecordingOrReplaying(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplayOnConsoleAPI(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplaySetCommandCallback(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplaySetClearPauseDataCallback(const FunctionCallbackInfo<Value>& callArgs);
extern void FunctionCallbackRecordReplayIgnoreScript(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplayAssert(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplayGetCurrentError(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplayGetRecordingId(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplayCurrentExecutionPoint(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplayElapsedTimeMs(const FunctionCallbackInfo<Value>& args);
extern void FunctionCallbackRecordReplayForceCheckpoint(const FunctionCallbackInfo<Value>& args);

}

namespace node {

using v8::Context;
using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::Local;
using v8::NewStringType;
using v8::Object;
using v8::V8;
using v8::Value;

static void (*gRecordReplayAttach)(const char* buildId);
static void (*gRecordReplayRecordCommandLineArguments)(int*, char***);
static size_t (*gRecordReplayMaxOldSpaceMb)();
static void (*gRecordReplaySaveRecording)(const char* dir);
static void (*gRecordReplayRememberRecording)();
static void (*gRecordReplayAddMetadata)(const char* metadata);
static void (*gRecordReplayFinishRecording)();
static void (*gBeginCallbackRegion)();
static void (*gEndCallbackRegion)();
static void (*gMarkNextDlopenAsAddon)(const char* name);
static bool (*gIsInsideInterceptedCallback)();
static const char* (*gRecordReplayGetRecordingId)();
static char* (*gGetUnusableRecordingReason)();
static void* (*gJSONCreateString)(const char*);
static void* (*gJSONCreateArray)(size_t, void**);
static void* (*gJSONCreateObject)(size_t, const char**, void**);
static char* (*gJSONToString)(void*);
static void (*gJSONFree)(void*);

namespace recordreplay {

static bool gRecordingFinished;

bool IsRecordingFinished() {
  return gRecordingFinished;
}

void BeginCallbackRegion() {
  if (v8::recordreplay::IsRecordingOrReplaying()) {
    gBeginCallbackRegion();
  }
}

void EndCallbackRegion() {
  if (v8::recordreplay::IsRecordingOrReplaying()) {
    gEndCallbackRegion();
  }
}

void MarkNextDlopenAsAddon(const char* name) {
  if (v8::recordreplay::IsRecordingOrReplaying()) {
    gMarkNextDlopenAsAddon(name);
  }
}

bool IsInsideInterceptedCallback() {
  return v8::recordreplay::IsRecordingOrReplaying() &&
         gIsInsideInterceptedCallback();
}

bool AreEventsAvailable() {
  return v8::recordreplay::IsRecordingOrReplaying() &&
         !v8::recordreplay::AreEventsDisallowed() &&
         !v8::recordreplay::HasDivergedFromRecording();
}

bool AreEventsRecorded() {
  return AreEventsAvailable() && !v8::recordreplay::AreEventsPassedThrough();
}

bool AreEventsUnavailable() {
  return v8::recordreplay::IsRecordingOrReplaying() && !AreEventsAvailable();
}

bool ThrowIfEventsUnavailable(v8::Isolate* isolate,
                              const char* operation_name) {
  if (!AreEventsUnavailable()) {
    return false;
  }
  std::string message = std::string("Cannot replay operation ") +
                         operation_name + " because it was not recorded.";
  v8::HandleScope handle_scope(isolate);
  isolate->ThrowException(
      v8::Exception::TypeError(OneByteString(isolate, message.c_str())));
  return true;
}

bool ThrowIfEventsUnavailable(Environment* env, const char* operation_name) {
  return ThrowIfEventsUnavailable(env->isolate(), operation_name);
}

bool ThrowIfEventsUnavailable(const v8::FunctionCallbackInfo<v8::Value>& args,
                              const char* operation_name) {
  return ThrowIfEventsUnavailable(Environment::GetCurrent(args),
                                  operation_name);
}

} // namespace recordreplay

void RecordReplayFinishRecording() {
  if (gRecordReplayFinishRecording) {
    gRecordReplayFinishRecording();
    recordreplay::gRecordingFinished = true;

    // If specified via the environment, append the recording ID to a file.
    const char* env = getenv("RECORD_REPLAY_RECORDING_ID_FILE");
    if (env) {
      // Don't add the recording ID when the recording is unusable.
      char* reason = gGetUnusableRecordingReason();
      if (reason) {
        free(reason);
        return;
      }

      FILE* file = fopen(env, "a");
      if (file) {
        const char* recordingId = gRecordReplayGetRecordingId();
        fprintf(file, "%s\n", recordingId);
        fclose(file);
      } else {
        fprintf(stderr, "Error: Could not open %s for adding recording ID", env);
      }
    }
  }
}

template <typename Src, typename Dst>
static inline void CastPointer(const Src src, Dst* dst) {
  static_assert(sizeof(Src) == sizeof(uintptr_t), "bad size");
  static_assert(sizeof(Dst) == sizeof(uintptr_t), "bad size");
  memcpy((void*)dst, (const void*)&src, sizeof(uintptr_t));
}

template <typename T>
static void RecordReplayLoadSymbol(void* handle, const char* name, T& function) {
  void* sym = dlsym(handle, name);
  if (!sym) {
    fprintf(stderr, "Could not find %s in Record Replay driver.\n", name);
    return;
  }

  CastPointer(sym, &function);
}

extern char gBuildId[];
extern char gRecordReplayDriver[];
extern int gRecordReplayDriverSize;

static std::string GetRecordingMetadata(int argc, char** argv) {
  std::vector<void*> handles;
  for (int i = 1; i < argc; i++) {
    handles.push_back(gJSONCreateString(argv[i]));
  }
  void* array = gJSONCreateArray(handles.size(), &handles[0]);
  const char* property = "argv";

  void* object = gJSONCreateObject(1, &property, &array);
  char* objectStr = gJSONToString(object);
  std::string rv = objectStr;

  free(objectStr);
  gJSONFree(object);
  gJSONFree(array);
  for (void* handle : handles) {
    gJSONFree(handle);
  }

  return rv;
}

static void* OpenDriverHandle() {
  const char* driver = getenv("RECORD_REPLAY_DRIVER");
  bool temporaryDriver = false;

  if (!driver) {
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) {
      tmpdir = "/tmp";
    }

    char filename[1024];
    snprintf(filename, sizeof(filename), "%s/recordreplay.so-XXXXXX", tmpdir);
    int fd = mkstemp(filename);
    if (fd < 0) {
      fprintf(stderr, "mkstemp failed, can't create driver.\n");
      return nullptr;
    }

    int nbytes = write(fd, gRecordReplayDriver, gRecordReplayDriverSize);
    if (nbytes != gRecordReplayDriverSize) {
      fprintf(stderr, "write to driver temporary file failed, can't create driver.\n");
      return nullptr;
    }

    temporaryDriver = true;
    driver = strdup(filename);
    close(fd);
  }

  void* handle = dlopen(driver, RTLD_LAZY);

  if (temporaryDriver) {
    unlink(driver);
  }

  return handle;
}

// The driver can give the JS heap a different old space limit when recording
// and when replaying, e.g. to make the replay's GC run at other points. It is
// applied as a V8 flag after the command line's, which it overrides, so that
// the arguments (and process.execArgv) stay the same when replaying.
void ApplyRecordReplayMaxOldSpace() {
  if (!v8::recordreplay::IsRecordingOrReplaying()) {
    return;
  }
  size_t mb = gRecordReplayMaxOldSpaceMb();
  if (!mb) {
    return;
  }
  std::string flag = "--max-old-space-size=" + std::to_string(mb);
  V8::SetFlagsFromString(flag.c_str(), flag.size());
}

// x64 codegen selects on two CPUID leaf 7 sub-leaf 1 features, AVX-VNNI and
// AVX-VNNI-INT8, which it reads from whichever CPU the process is running on.
// Left alone they make a replay generate different code than the recording
// did, and the wasm relaxed dot products are where that shows: with either
// feature the op becomes one vpdpbssd or vpdpbusd, and without them a
// saturating vpmaddubsw and pmaddwd pair. The three disagree once the i7
// operand's high bit is set, and they differ in length, which moves when
// Liftoff asks to tier up even when the results agree.
//
// Recording the leaf would not fix this. It would make both sides agree about
// what the CPU had, but these features gate whether an instruction may be
// emitted at all, so a recording from a machine with AVX-VNNI would have the
// replay emit vpdpbusd or vpdpbssd on a host that need not implement it,
// trading a divergence for an illegal instruction. A replay runs wherever
// there is capacity and cannot be promised to be as new as every recording's
// machine. Turning both features off for recording and replaying alike is what
// makes the generated code the same, whatever either CPU can do: the only ops
// affected are those dot products, and the operands that tell the lowerings
// apart are the ones the opcodes leave implementation-defined, so no
// conforming program can see which one it got.
//
// This runs before V8::Initialize(), which is what probes the CPU, and like
// the old space limit above it is applied as a V8 flag after the command
// line's so that the arguments stay the same when replaying.
void ApplyRecordReplayCpuFeatures() {
  if (!v8::recordreplay::IsRecordingOrReplaying()) {
    return;
  }
  static const char kFlags[] = "--no-enable-avx-vnni --no-enable-avx-vnni-int8";
  V8::SetFlagsFromString(kFlags, sizeof(kFlags) - 1);
}

// Whether the command line asks node to build a V8 startup snapshot, which is
// not recorded (see InitializeRecordReplay). --build-snapshot and
// --build-snapshot-config are not allowed in NODE_OPTIONS, so the command line
// is their only source, and node only parses it later: this runs before the
// driver rewrites the arguments for a replay. Every argument is looked at, so a
// program whose own arguments contain --build-snapshot is not recorded either.
static bool BuildingStartupSnapshot(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    // Covers --build-snapshot-config, which implies --build-snapshot, in both
    // its --opt=value and --opt value forms.
    if (!strncmp(argv[i], "--build-snapshot", strlen("--build-snapshot"))) {
      return true;
    }
  }
  return false;
}

void InitializeRecordReplay(int* pargc, char*** pargv) {
  if (getenv("RECORD_REPLAY_DONT_RECORD")) {
    return;
  }

  // Building a startup snapshot cannot be recorded. While recording, every
  // script that compiles is kept in an eternal handle so the debugger can find
  // it again, and V8's snapshot serializer requires every eternal, global and
  // traced handle alive at serialization to have been registered for
  // serialization, so the build dies in CheckGlobalAndEternalHandles. Eternal
  // handles cannot be released, and the registry they belong to is process
  // state that no snapshot carries, so there is nothing to register them as.
  //
  // An invalidated recording is not enough: invalidating discards a recording
  // but leaves the instrumentation that creates those handles running, so the
  // build still dies. The driver is not loaded at all instead, which also means
  // nothing can report the reason, so it is said here. Processes started from
  // the snapshot record as usual.
  if (BuildingStartupSnapshot(*pargc, *pargv)) {
    fprintf(stderr,
            "Building a V8 startup snapshot is not recorded.\n");
    return;
  }

  void* handle = OpenDriverHandle();

  if (!handle) {
    fprintf(stderr, "Loading Record Replay driver failed (%s).\n", dlerror());
    return;
  }

  RecordReplayLoadSymbol(handle, "RecordReplayAttach", gRecordReplayAttach);
  RecordReplayLoadSymbol(handle, "RecordReplayRecordCommandLineArguments",
                         gRecordReplayRecordCommandLineArguments);
  RecordReplayLoadSymbol(handle, "RecordReplayMaxOldSpaceMb",
                         gRecordReplayMaxOldSpaceMb);
  RecordReplayLoadSymbol(handle, "RecordReplaySaveRecording", gRecordReplaySaveRecording);
  RecordReplayLoadSymbol(handle, "RecordReplayRememberRecording", gRecordReplayRememberRecording);
  RecordReplayLoadSymbol(handle, "RecordReplayAddMetadata", gRecordReplayAddMetadata);
  RecordReplayLoadSymbol(handle, "RecordReplayFinishRecording", gRecordReplayFinishRecording);
  RecordReplayLoadSymbol(handle, "RecordReplayGetRecordingId", gRecordReplayGetRecordingId);
  RecordReplayLoadSymbol(handle, "RecordReplayGetUnusableRecordingReason", gGetUnusableRecordingReason);
  RecordReplayLoadSymbol(handle, "RecordReplayBeginCallbackRegion", gBeginCallbackRegion);
  RecordReplayLoadSymbol(handle, "RecordReplayEndCallbackRegion", gEndCallbackRegion);
  RecordReplayLoadSymbol(handle, "RecordReplayMarkNextDlopenAsAddon", gMarkNextDlopenAsAddon);
  RecordReplayLoadSymbol(handle, "RecordReplayIsInsideInterceptedCallback",
                         gIsInsideInterceptedCallback);
  RecordReplayLoadSymbol(handle, "RecordReplayJSONCreateString", gJSONCreateString);
  RecordReplayLoadSymbol(handle, "RecordReplayJSONCreateArray", gJSONCreateArray);
  RecordReplayLoadSymbol(handle, "RecordReplayJSONCreateObject", gJSONCreateObject);
  RecordReplayLoadSymbol(handle, "RecordReplayJSONToString", gJSONToString);
  RecordReplayLoadSymbol(handle, "RecordReplayJSONFree", gJSONFree);

  if (gRecordReplayAttach && gRecordReplayFinishRecording) {
    gRecordReplayAttach(gBuildId);
    gRecordReplayRecordCommandLineArguments(pargc, pargv);
    v8::recordreplay::SetRecordingOrReplaying(handle);

    if (gRecordReplaySaveRecording) {
      gRecordReplaySaveRecording(nullptr);
    }

    gRecordReplayRememberRecording();

    if (gRecordReplayAddMetadata) {
      std::string metadata = GetRecordingMetadata(*pargc, *pargv);
      gRecordReplayAddMetadata(metadata.c_str());
    }
  }
}

namespace process {

static void RecordReplayLog(const FunctionCallbackInfo<Value>& args) {
  CHECK(args.Length() == 1 && args[0]->IsString() &&
        "must be called with a single string");
  Utf8Value text(args.GetIsolate(), args[0]);
  v8::recordreplay::Print("%s", text.ToString().c_str());
}

static void RecordReplayAreEventsUnavailable(
    const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(recordreplay::AreEventsUnavailable());
}

// Function to invoke on CDP responses and events.
static v8::Eternal<v8::Function>* gCDPMessageCallback;

static void RecordReplaySetCDPMessageCallback(const FunctionCallbackInfo<Value>& args) {
  CHECK(!gCDPMessageCallback);
  Isolate* isolate = args.GetIsolate();
  CHECK(args[0]->IsFunction());
  Local<v8::Function> callback = args[0].As<v8::Function>();
  gCDPMessageCallback = new v8::Eternal<v8::Function>(isolate, callback);
}

static std::unique_ptr<inspector::InspectorSession> gRecordReplayInspectorSession;

class RecordReplaySessionDelegate : public inspector::InspectorSessionDelegate {
 public:
  void SendMessageToFrontend(const v8_inspector::StringView& message) override {
    v8::replayio::AutoMarkReplayCode mark;
    v8::replayio::AutoDisallowEvents disallow(
        "RecordReplay_SendMessageToFrontend");
    CHECK(v8::IsMainThread());

    if (recordreplay::IsRecordingFinished()) {
      return;
    }

    CHECK(gCDPMessageCallback);

    Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    Local<Context> context = isolate->GetCurrentContext();

    // The inspector hands protocol messages over as Latin-1 as well as UTF-16,
    // so let node's own conversion pick the factory that matches each one.
    Local<Value> arg = ToV8Value(context, message, isolate).ToLocalChecked();
    Local<v8::Function> callback = gCDPMessageCallback->Get(isolate);
    v8::MaybeLocal<Value> rv = callback->Call(context, v8::Undefined(isolate), 1, &arg);
    CHECK(!rv.IsEmpty());
  }
};

static void RecordReplaySendCDPMessage(const FunctionCallbackInfo<Value>& args) {
  CHECK(args.Length() == 1 && args[0]->IsString() &&
        "must be called with a single string");
  Utf8Value message(args.GetIsolate(), args[0]);

  if (!gRecordReplayInspectorSession) {
    Environment* env = Environment::GetCurrent(args);
    inspector::Agent* agent = env->inspector_agent();

    gRecordReplayInspectorSession = agent->RecordReplayConnect(
        std::make_unique<RecordReplaySessionDelegate>());
    // process.exit() skips Environment destruction, so the process-owned Replay
    // session would otherwise survive until static destruction, after V8's platform
    // has been disposed. Reset it from AtExit, where Node normally waits for external
    // debugger frontends to disconnect. This safely disables the debugger, which
    // leaves WebAssembly modules tiered down (see V8Debugger::disable).
    AtExit(env, [](void*) {
      gRecordReplayInspectorSession.reset();
    }, nullptr);
  }

  std::string nmessage(message.ToString());
  v8_inspector::StringView messageView((const uint8_t*)nmessage.c_str(), nmessage.length());
  {
    v8::replayio::AutoMarkReplayCode mark;
    v8::replayio::AutoDisallowEvents disallow("RecordReplaySendCDPMessage");
    gRecordReplayInspectorSession->Dispatch(messageView);
  }
}

// Called from javascript.
// `recordreplay.annotationHook(kind, contents)`
// Since this function is called from userland JS, we avoid assertions.
// We don't want flawed uses of the API to crash the recording.
static void RecordReplayAnnotationHook(
    const FunctionCallbackInfo<Value>& args) {
  if (!(args.Length() >= 2 && args[0]->IsString())) {
    v8::recordreplay::Print("[RuntimeError] %s called with incorrect arguments",
                            AnnotationHookJSName);
    return;
  }

  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<v8::Object> payload = v8::Object::New(isolate);
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  payload->Set(context, v8::String::NewFromUtf8(isolate, "message").ToLocalChecked(), args[1]).Check();

  v8::Local<v8::String> json;
  if (!v8::JSON::Stringify(context, payload).ToLocal(&json)) {
    v8::recordreplay::Print(
        "[RuntimeError] %s contents failed to json stringify",
        AnnotationHookJSName);
    return;
  }

  v8::String::Utf8Value kind(args.GetIsolate(), args[0]);
  v8::String::Utf8Value contents(args.GetIsolate(), json);
  v8::recordreplay::OnAnnotation(*kind, *contents);
}

// Network capture for the recording's Network panel
// (lib/internal/recordreplay/network.js). It runs on the main thread only, as
// in Chromium: worker threads aren't debuggable in Replay, so their requests
// aren't reported.
static void RecordReplayNetworkEnabled(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(
      v8::recordreplay::IsRecordingOrReplaying("notify-network") &&
      v8::IsMainThread());
}

// Registers a request at the current execution point, which becomes the point
// the request was made at.
static void RecordReplayNetworkRequest(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString());
  Utf8Value id(args.GetIsolate(), args[0]);
  uint64_t bookmark = v8::recordreplay::NewBookmark();
  v8::recordreplay::OnNetworkRequest(*id, "http", bookmark);
}

// While replaying, the driver asks for the event's contents from inside this
// call, with Target.getCurrentNetworkRequestEvent.
static void RecordReplayNetworkRequestEvent(
    const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString());
  Utf8Value id(args.GetIsolate(), args[0]);
  v8::recordreplay::OnNetworkRequestEvent(*id);
}

static void RecordReplayNetworkStreamStart(
    const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString() && args[1]->IsString() && args[2]->IsString());
  Isolate* isolate = args.GetIsolate();
  Utf8Value id(isolate, args[0]);
  Utf8Value kind(isolate, args[1]);
  Utf8Value parent_id(isolate, args[2]);
  v8::recordreplay::OnNetworkStreamStart(*id, *kind, *parent_id);
}

// While replaying, the driver asks for the chunk's bytes from inside this
// call, with Target.getCurrentNetworkStreamData.
static void RecordReplayNetworkStreamData(
    const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString() && args[1]->IsNumber() && args[2]->IsNumber());
  Utf8Value id(args.GetIsolate(), args[0]);
  size_t offset = args[1].As<v8::Number>()->Value();
  size_t length = args[2].As<v8::Number>()->Value();
  v8::recordreplay::OnNetworkStreamData(*id, offset, length, 0);
}

// The innermost frame of the program's own code on the current stack, as a
// protocol RequestInitiator ({ url, line, column }), or undefined. Node's
// scripts ("node:..." URLs) are skipped, so the frame is the call that made
// the request. It walks V8's stack itself, so the program's
// Error.prepareStackTrace and Error.stackTraceLimit aren't involved.
static void RecordReplayNetworkInitiator(
    const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  // Enough for the frames of Replay's command handling, undici's dispatch and
  // node's own wrappers above the program's.
  constexpr int kMaxFrames = 200;
  Local<v8::StackTrace> trace = v8::StackTrace::CurrentStackTrace(
      isolate, kMaxFrames, v8::StackTrace::kDetailed);
  for (int i = 0; i < trace->GetFrameCount(); i++) {
    Local<v8::StackFrame> frame = trace->GetFrame(isolate, i);
    Local<v8::String> name = frame->GetScriptNameOrSourceURL();
    if (name.IsEmpty() || name->Length() == 0) {
      continue;
    }
    Utf8Value url(isolate, name);
    if (strncmp(*url, "node:", 5) == 0) {
      continue;
    }
    Local<Object> initiator = Object::New(isolate);
    initiator->Set(context, FIXED_ONE_BYTE_STRING(isolate, "url"), name).Check();
    // The protocol's line is 1-based and its column 0-based.
    initiator
        ->Set(context, FIXED_ONE_BYTE_STRING(isolate, "line"),
              v8::Integer::New(isolate, frame->GetLineNumber()))
        .Check();
    initiator
        ->Set(context, FIXED_ONE_BYTE_STRING(isolate, "column"),
              v8::Integer::New(isolate, frame->GetColumn() - 1))
        .Check();
    args.GetReturnValue().Set(initiator);
    return;
  }
}

void InitializeRecordReplayMethods(Environment* env, Local<Object> target) {
  Local<Context> context = env->context();
  SetMethod(context, target, "isRecordingOrReplaying",
            v8::FunctionCallbackIsRecordingOrReplaying);
  SetMethod(context, target, "recordReplayLog", RecordReplayLog);
  SetMethod(context, target, "recordReplayAreEventsUnavailable",
            RecordReplayAreEventsUnavailable);
  SetMethod(context, target, "recordReplayOnConsoleAPI",
            v8::FunctionCallbackRecordReplayOnConsoleAPI);
  SetMethod(context, target, "recordReplaySetCommandCallback",
            v8::FunctionCallbackRecordReplaySetCommandCallback);
  SetMethod(context, target, "recordReplaySetClearPauseDataCallback",
            v8::FunctionCallbackRecordReplaySetClearPauseDataCallback);
  SetMethod(context, target, "recordReplayIgnoreScript",
            v8::FunctionCallbackRecordReplayIgnoreScript);
  SetMethod(context, target, "recordReplayAssert",
            v8::FunctionCallbackRecordReplayAssert);
  SetMethod(context, target, "recordReplayGetCurrentError",
            v8::FunctionCallbackRecordReplayGetCurrentError);
  SetMethod(context, target, "recordReplaySetCDPMessageCallback",
            RecordReplaySetCDPMessageCallback);
  SetMethod(context, target, "recordReplaySendCDPMessage",
            RecordReplaySendCDPMessage);
  SetMethod(context, target, "recordReplayRecordingId",
            v8::FunctionCallbackRecordReplayGetRecordingId);
  SetMethod(context, target, "recordReplayCurrentExecutionPoint",
            v8::FunctionCallbackRecordReplayCurrentExecutionPoint);
  SetMethod(context, target, "recordReplayElapsedTimeMs",
            v8::FunctionCallbackRecordReplayElapsedTimeMs);
  SetMethod(context, target, "recordReplayForceCheckpoint",
            v8::FunctionCallbackRecordReplayForceCheckpoint);
  SetMethod(context, target, "recordReplayAnnotationHook",
            RecordReplayAnnotationHook);
  SetMethod(context, target, "recordReplayNetworkEnabled",
            RecordReplayNetworkEnabled);
  SetMethod(context, target, "recordReplayNetworkRequest",
            RecordReplayNetworkRequest);
  SetMethod(context, target, "recordReplayNetworkRequestEvent",
            RecordReplayNetworkRequestEvent);
  SetMethod(context, target, "recordReplayNetworkStreamStart",
            RecordReplayNetworkStreamStart);
  SetMethod(context, target, "recordReplayNetworkStreamData",
            RecordReplayNetworkStreamData);
  SetMethod(context, target, "recordReplayNetworkInitiator",
            RecordReplayNetworkInitiator);
}

void RegisterRecordReplayExternalReferences(
    ExternalReferenceRegistry* registry) {
  registry->Register(v8::FunctionCallbackIsRecordingOrReplaying);
  registry->Register(RecordReplayLog);
  registry->Register(RecordReplayAreEventsUnavailable);
  registry->Register(v8::FunctionCallbackRecordReplayOnConsoleAPI);
  registry->Register(v8::FunctionCallbackRecordReplaySetCommandCallback);
  registry->Register(v8::FunctionCallbackRecordReplaySetClearPauseDataCallback);
  registry->Register(v8::FunctionCallbackRecordReplayIgnoreScript);
  registry->Register(v8::FunctionCallbackRecordReplayAssert);
  registry->Register(v8::FunctionCallbackRecordReplayGetCurrentError);
  registry->Register(RecordReplaySetCDPMessageCallback);
  registry->Register(RecordReplaySendCDPMessage);
  registry->Register(v8::FunctionCallbackRecordReplayGetRecordingId);
  registry->Register(v8::FunctionCallbackRecordReplayCurrentExecutionPoint);
  registry->Register(v8::FunctionCallbackRecordReplayElapsedTimeMs);
  registry->Register(v8::FunctionCallbackRecordReplayForceCheckpoint);
  registry->Register(RecordReplayAnnotationHook);
  registry->Register(RecordReplayNetworkEnabled);
  registry->Register(RecordReplayNetworkRequest);
  registry->Register(RecordReplayNetworkRequestEvent);
  registry->Register(RecordReplayNetworkStreamStart);
  registry->Register(RecordReplayNetworkStreamData);
  registry->Register(RecordReplayNetworkInitiator);
}

}  // namespace process
}  // namespace node
