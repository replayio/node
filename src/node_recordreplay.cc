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

bool ThrowIfEventsUnavailable(Environment* env, const char* operation_name) {
  if (!AreEventsUnavailable()) {
    return false;
  }
  std::string message = std::string("Cannot replay operation ") +
                         operation_name + " because it was not recorded.";
  env->ThrowTypeError(message.c_str());
  return true;
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

void InitializeRecordReplay(int* pargc, char*** pargv) {
  if (getenv("RECORD_REPLAY_DONT_RECORD")) {
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
    CHECK(!message.is8Bit());

    Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    Local<Context> context = isolate->GetCurrentContext();

    Local<Value> arg = v8::String::NewFromTwoByte(isolate, message.characters16(),
                                                  NewStringType::kNormal,
                                                  message.length()).ToLocalChecked();
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

    auto delegate = std::make_unique<RecordReplaySessionDelegate>();
    {
      v8::replayio::AutoMarkReplayCode mark;
      v8::replayio::AutoDisallowEvents disallow(
          "RecordReplaySendCDPMessage::Connect");
      gRecordReplayInspectorSession = agent->Connect(std::move(delegate),
                                                     /* prevent_shutdown */ false);
    }
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

void InitializeRecordReplayMethods(Environment* env, Local<Object> target) {
  env->SetMethod(target, "isRecordingOrReplaying",
                 v8::FunctionCallbackIsRecordingOrReplaying);
  env->SetMethod(target, "recordReplayLog", RecordReplayLog);
  env->SetMethod(target,
                 "recordReplayAreEventsUnavailable",
                 RecordReplayAreEventsUnavailable);
  env->SetMethod(target, "recordReplayOnConsoleAPI",
                 v8::FunctionCallbackRecordReplayOnConsoleAPI);
  env->SetMethod(target, "recordReplaySetCommandCallback",
                 v8::FunctionCallbackRecordReplaySetCommandCallback);
  env->SetMethod(target, "recordReplaySetClearPauseDataCallback",
                 v8::FunctionCallbackRecordReplaySetClearPauseDataCallback);
  env->SetMethod(target, "recordReplayIgnoreScript",
                 v8::FunctionCallbackRecordReplayIgnoreScript);
  env->SetMethod(target, "recordReplayAssert",
                 v8::FunctionCallbackRecordReplayAssert);
  env->SetMethod(target, "recordReplayGetCurrentError",
                 v8::FunctionCallbackRecordReplayGetCurrentError);
  env->SetMethod(target, "recordReplaySetCDPMessageCallback",
                 RecordReplaySetCDPMessageCallback);
  env->SetMethod(target, "recordReplaySendCDPMessage",
                 RecordReplaySendCDPMessage);
  env->SetMethod(target, "recordReplayRecordingId",
                 v8::FunctionCallbackRecordReplayGetRecordingId);
  env->SetMethod(target, "recordReplayCurrentExecutionPoint",
                 v8::FunctionCallbackRecordReplayCurrentExecutionPoint);
  env->SetMethod(target, "recordReplayElapsedTimeMs",
                 v8::FunctionCallbackRecordReplayElapsedTimeMs);
  env->SetMethod(
      target, "recordReplayAnnotationHook", RecordReplayAnnotationHook);
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
  registry->Register(RecordReplayAnnotationHook);
}

}  // namespace process
}  // namespace node
