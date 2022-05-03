#include "node_perf.h"
#include "aliased_buffer.h"
#include "env-inl.h"
#include "histogram-inl.h"
#include "memory_tracker-inl.h"
#include "node_buffer.h"
#include "node_external_reference.h"
#include "node_internals.h"
#include "node_process-inl.h"
#include "util-inl.h"

#include <cinttypes>

namespace node {

namespace performance {

using v8::Context;
using v8::DontDelete;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::GCCallbackFlags;
using v8::GCType;
using v8::Int32;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::PropertyAttribute;
using v8::ReadOnly;
using v8::String;
using v8::Value;

// Microseconds in a millisecond, as a float.
#define MICROS_PER_MILLIS 1e3

// https://w3c.github.io/hr-time/#dfn-time-origin
uint64_t timeOrigin;
// https://w3c.github.io/hr-time/#dfn-time-origin-timestamp
double timeOriginTimestamp;
uint64_t performance_v8_start;

void InitPerformance() {
  // Don't initialize these statically so they will have consistent values when
  // recording/replaying.
  timeOrigin = PERFORMANCE_NOW();
  timeOriginTimestamp = GetCurrentTimeInMicroseconds();
}

PerformanceState::PerformanceState(Isolate* isolate,
                                   const PerformanceState::SerializeInfo* info)
    : root(isolate,
           sizeof(performance_state_internal),
           MAYBE_FIELD_PTR(info, root)),
      milestones(isolate,
                 offsetof(performance_state_internal, milestones),
                 NODE_PERFORMANCE_MILESTONE_INVALID,
                 root,
                 MAYBE_FIELD_PTR(info, milestones)),
      observers(isolate,
                offsetof(performance_state_internal, observers),
                NODE_PERFORMANCE_ENTRY_TYPE_INVALID,
                root,
                MAYBE_FIELD_PTR(info, observers)) {
  if (info == nullptr) {
    for (size_t i = 0; i < milestones.Length(); i++) milestones[i] = -1.;
  }
}

PerformanceState::SerializeInfo PerformanceState::Serialize(
    v8::Local<v8::Context> context, v8::SnapshotCreator* creator) {
  SerializeInfo info{root.Serialize(context, creator),
                     milestones.Serialize(context, creator),
                     observers.Serialize(context, creator)};
  return info;
}

void PerformanceState::Deserialize(v8::Local<v8::Context> context) {
  root.Deserialize(context);
  // This is just done to set up the pointers, we will actually reset
  // all the milestones after deserialization.
  milestones.Deserialize(context);
  observers.Deserialize(context);
}

std::ostream& operator<<(std::ostream& o,
                         const PerformanceState::SerializeInfo& i) {
  o << "{\n"
    << "  " << i.root << ",  // root\n"
    << "  " << i.milestones << ",  // milestones\n"
    << "  " << i.observers << ",  // observers\n"
    << "}";
  return o;
}

void PerformanceState::Mark(PerformanceMilestone milestone, uint64_t ts) {
  this->milestones[milestone] = static_cast<double>(ts);
  TRACE_EVENT_INSTANT_WITH_TIMESTAMP0(
      TRACING_CATEGORY_NODE1(bootstrap),
      GetPerformanceMilestoneName(milestone),
      TRACE_EVENT_SCOPE_THREAD, ts / 1000);
}

<<<<<<< HEAD
// Initialize the performance entry object properties
inline void InitObject(const PerformanceEntry& entry, Local<Object> obj) {
  Environment* env = entry.env();
  Isolate* isolate = env->isolate();
  Local<Context> context = env->context();
  PropertyAttribute attr =
      static_cast<PropertyAttribute>(ReadOnly | DontDelete);
  obj->DefineOwnProperty(context,
                         env->name_string(),
                         String::NewFromUtf8(isolate,
                                             entry.name().c_str())
                             .ToLocalChecked(),
                         attr)
      .Check();
  obj->DefineOwnProperty(context,
                         env->entry_type_string(),
                         String::NewFromUtf8(isolate,
                                             entry.type().c_str())
                             .ToLocalChecked(),
                         attr)
      .Check();
  obj->DefineOwnProperty(context,
                         env->start_time_string(),
                         Number::New(isolate, entry.startTime()),
                         attr).Check();
  obj->DefineOwnProperty(context,
                         env->duration_string(),
                         Number::New(isolate, entry.duration()),
                         attr).Check();
}

// Create a new PerformanceEntry object
MaybeLocal<Object> PerformanceEntry::ToObject() const {
  Local<Object> obj;
  if (!env_->performance_entry_template()
           ->NewInstance(env_->context())
           .ToLocal(&obj)) {
    return MaybeLocal<Object>();
  }
  InitObject(*this, obj);
  return obj;
}

// Allow creating a PerformanceEntry object from JavaScript
void PerformanceEntry::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  Utf8Value name(isolate, args[0]);
  Utf8Value type(isolate, args[1]);
  uint64_t now = PERFORMANCE_NOW();
  PerformanceEntry entry(env, *name, *type, now, now);
  Local<Object> obj = args.This();
  InitObject(entry, obj);
  PerformanceEntry::Notify(env, entry.kind(), obj);
}

// Pass the PerformanceEntry object to the PerformanceObservers
void PerformanceEntry::Notify(Environment* env,
                              PerformanceEntryType type,
                              Local<Value> object) {
  Context::Scope scope(env->context());
  AliasedUint32Array& observers = env->performance_state()->observers;
  if (!env->performance_entry_callback().IsEmpty() &&
      type != NODE_PERFORMANCE_ENTRY_TYPE_INVALID && observers[type]) {
    // Performance entries can be non-deterministic and are not currently
    // supported when recording/replaying.
    v8::recordreplay::InvalidateRecording("Performance entries observed");
    node::MakeCallback(env->isolate(),
                       object.As<Object>(),
                       env->performance_entry_callback(),
                       1, &object,
                       node::async_context{0, 0});
  }
}

// Create a User Timing Mark
void Mark(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  HandleScope scope(env->isolate());
  Utf8Value name(env->isolate(), args[0]);
  uint64_t now = PERFORMANCE_NOW();
  auto marks = env->performance_marks();
  (*marks)[*name] = now;

  TRACE_EVENT_COPY_MARK_WITH_TIMESTAMP(
      TRACING_CATEGORY_NODE2(perf, usertiming),
      *name, now / 1000);

  PerformanceEntry entry(env, *name, "mark", now, now);
  Local<Object> obj;
  if (!entry.ToObject().ToLocal(&obj)) return;
  PerformanceEntry::Notify(env, entry.kind(), obj);
  args.GetReturnValue().Set(obj);
}

void ClearMark(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  auto marks = env->performance_marks();

  if (args.Length() == 0) {
    marks->clear();
  } else {
    Utf8Value name(env->isolate(), args[0]);
    marks->erase(*name);
  }
}

inline uint64_t GetPerformanceMark(Environment* env, const std::string& name) {
  auto marks = env->performance_marks();
  auto res = marks->find(name);
  return res != marks->end() ? res->second : 0;
}

// Create a User Timing Measure. A Measure is a PerformanceEntry that
// measures the duration between two distinct user timing marks
void Measure(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  HandleScope scope(env->isolate());
  Utf8Value name(env->isolate(), args[0]);
  Utf8Value startMark(env->isolate(), args[1]);

  AliasedFloat64Array& milestones = env->performance_state()->milestones;

  uint64_t startTimestamp = timeOrigin;
  uint64_t start = GetPerformanceMark(env, *startMark);
  if (start != 0) {
    startTimestamp = start;
  } else {
    PerformanceMilestone milestone = ToPerformanceMilestoneEnum(*startMark);
    if (milestone != NODE_PERFORMANCE_MILESTONE_INVALID)
      startTimestamp = milestones[milestone];
  }

  uint64_t endTimestamp = 0;
  if (args[2]->IsUndefined()) {
    endTimestamp = PERFORMANCE_NOW();
  } else {
    Utf8Value endMark(env->isolate(), args[2]);
    endTimestamp = GetPerformanceMark(env, *endMark);
    if (endTimestamp == 0) {
      PerformanceMilestone milestone = ToPerformanceMilestoneEnum(*endMark);
      if (milestone != NODE_PERFORMANCE_MILESTONE_INVALID)
        endTimestamp = milestones[milestone];
    }
  }

  if (endTimestamp < startTimestamp)
    endTimestamp = startTimestamp;

  TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP0(
      TRACING_CATEGORY_NODE2(perf, usertiming),
      *name, *name, startTimestamp / 1000);
  TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(
      TRACING_CATEGORY_NODE2(perf, usertiming),
      *name, *name, endTimestamp / 1000);

  PerformanceEntry entry(env, *name, "measure", startTimestamp, endTimestamp);
  Local<Object> obj;
  if (!entry.ToObject().ToLocal(&obj)) return;
  PerformanceEntry::Notify(env, entry.kind(), obj);
  args.GetReturnValue().Set(obj);
}

||||||| 2365115868
// Initialize the performance entry object properties
inline void InitObject(const PerformanceEntry& entry, Local<Object> obj) {
  Environment* env = entry.env();
  Isolate* isolate = env->isolate();
  Local<Context> context = env->context();
  PropertyAttribute attr =
      static_cast<PropertyAttribute>(ReadOnly | DontDelete);
  obj->DefineOwnProperty(context,
                         env->name_string(),
                         String::NewFromUtf8(isolate,
                                             entry.name().c_str())
                             .ToLocalChecked(),
                         attr)
      .Check();
  obj->DefineOwnProperty(context,
                         env->entry_type_string(),
                         String::NewFromUtf8(isolate,
                                             entry.type().c_str())
                             .ToLocalChecked(),
                         attr)
      .Check();
  obj->DefineOwnProperty(context,
                         env->start_time_string(),
                         Number::New(isolate, entry.startTime()),
                         attr).Check();
  obj->DefineOwnProperty(context,
                         env->duration_string(),
                         Number::New(isolate, entry.duration()),
                         attr).Check();
}

// Create a new PerformanceEntry object
MaybeLocal<Object> PerformanceEntry::ToObject() const {
  Local<Object> obj;
  if (!env_->performance_entry_template()
           ->NewInstance(env_->context())
           .ToLocal(&obj)) {
    return MaybeLocal<Object>();
  }
  InitObject(*this, obj);
  return obj;
}

// Allow creating a PerformanceEntry object from JavaScript
void PerformanceEntry::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  Utf8Value name(isolate, args[0]);
  Utf8Value type(isolate, args[1]);
  uint64_t now = PERFORMANCE_NOW();
  PerformanceEntry entry(env, *name, *type, now, now);
  Local<Object> obj = args.This();
  InitObject(entry, obj);
  PerformanceEntry::Notify(env, entry.kind(), obj);
}

// Pass the PerformanceEntry object to the PerformanceObservers
void PerformanceEntry::Notify(Environment* env,
                              PerformanceEntryType type,
                              Local<Value> object) {
  Context::Scope scope(env->context());
  AliasedUint32Array& observers = env->performance_state()->observers;
  if (!env->performance_entry_callback().IsEmpty() &&
      type != NODE_PERFORMANCE_ENTRY_TYPE_INVALID && observers[type]) {
    node::MakeCallback(env->isolate(),
                       object.As<Object>(),
                       env->performance_entry_callback(),
                       1, &object,
                       node::async_context{0, 0});
  }
}

// Create a User Timing Mark
void Mark(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  HandleScope scope(env->isolate());
  Utf8Value name(env->isolate(), args[0]);
  uint64_t now = PERFORMANCE_NOW();
  auto marks = env->performance_marks();
  (*marks)[*name] = now;

  TRACE_EVENT_COPY_MARK_WITH_TIMESTAMP(
      TRACING_CATEGORY_NODE2(perf, usertiming),
      *name, now / 1000);

  PerformanceEntry entry(env, *name, "mark", now, now);
  Local<Object> obj;
  if (!entry.ToObject().ToLocal(&obj)) return;
  PerformanceEntry::Notify(env, entry.kind(), obj);
  args.GetReturnValue().Set(obj);
}

void ClearMark(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  auto marks = env->performance_marks();

  if (args.Length() == 0) {
    marks->clear();
  } else {
    Utf8Value name(env->isolate(), args[0]);
    marks->erase(*name);
  }
}

inline uint64_t GetPerformanceMark(Environment* env, const std::string& name) {
  auto marks = env->performance_marks();
  auto res = marks->find(name);
  return res != marks->end() ? res->second : 0;
}

// Create a User Timing Measure. A Measure is a PerformanceEntry that
// measures the duration between two distinct user timing marks
void Measure(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  HandleScope scope(env->isolate());
  Utf8Value name(env->isolate(), args[0]);
  Utf8Value startMark(env->isolate(), args[1]);

  AliasedFloat64Array& milestones = env->performance_state()->milestones;

  uint64_t startTimestamp = timeOrigin;
  uint64_t start = GetPerformanceMark(env, *startMark);
  if (start != 0) {
    startTimestamp = start;
  } else {
    PerformanceMilestone milestone = ToPerformanceMilestoneEnum(*startMark);
    if (milestone != NODE_PERFORMANCE_MILESTONE_INVALID)
      startTimestamp = milestones[milestone];
  }

  uint64_t endTimestamp = 0;
  if (args[2]->IsUndefined()) {
    endTimestamp = PERFORMANCE_NOW();
  } else {
    Utf8Value endMark(env->isolate(), args[2]);
    endTimestamp = GetPerformanceMark(env, *endMark);
    if (endTimestamp == 0) {
      PerformanceMilestone milestone = ToPerformanceMilestoneEnum(*endMark);
      if (milestone != NODE_PERFORMANCE_MILESTONE_INVALID)
        endTimestamp = milestones[milestone];
    }
  }

  if (endTimestamp < startTimestamp)
    endTimestamp = startTimestamp;

  TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP0(
      TRACING_CATEGORY_NODE2(perf, usertiming),
      *name, *name, startTimestamp / 1000);
  TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(
      TRACING_CATEGORY_NODE2(perf, usertiming),
      *name, *name, endTimestamp / 1000);

  PerformanceEntry entry(env, *name, "measure", startTimestamp, endTimestamp);
  Local<Object> obj;
  if (!entry.ToObject().ToLocal(&obj)) return;
  PerformanceEntry::Notify(env, entry.kind(), obj);
  args.GetReturnValue().Set(obj);
}

=======
>>>>>>> upstream/v16.x
// Allows specific Node.js lifecycle milestones to be set from JavaScript
void MarkMilestone(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  PerformanceMilestone milestone =
      static_cast<PerformanceMilestone>(args[0].As<Int32>()->Value());
  if (milestone != NODE_PERFORMANCE_MILESTONE_INVALID)
    env->performance_state()->Mark(milestone);
}

void SetupPerformanceObservers(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsFunction());
  env->set_performance_entry_callback(args[0].As<Function>());
}

// Marks the start of a GC cycle
void MarkGarbageCollectionStart(
    Isolate* isolate,
    GCType type,
    GCCallbackFlags flags,
    void* data) {
  Environment* env = static_cast<Environment*>(data);
  env->performance_state()->performance_last_gc_start_mark = PERFORMANCE_NOW();
}

MaybeLocal<Object> GCPerformanceEntryTraits::GetDetails(
    Environment* env,
    const GCPerformanceEntry& entry) {
  Local<Object> obj = Object::New(env->isolate());

  if (!obj->Set(
          env->context(),
          env->kind_string(),
          Integer::NewFromUnsigned(
              env->isolate(),
              entry.details.kind)).IsJust()) {
    return MaybeLocal<Object>();
  }

  if (!obj->Set(
          env->context(),
          env->flags_string(),
          Integer::NewFromUnsigned(
              env->isolate(),
              entry.details.flags)).IsJust()) {
    return MaybeLocal<Object>();
  }

  return obj;
}

// Marks the end of a GC cycle
void MarkGarbageCollectionEnd(
    Isolate* isolate,
    GCType type,
    GCCallbackFlags flags,
    void* data) {
  Environment* env = static_cast<Environment*>(data);
  PerformanceState* state = env->performance_state();
  // If no one is listening to gc performance entries, do not create them.
  if (LIKELY(!state->observers[NODE_PERFORMANCE_ENTRY_TYPE_GC]))
    return;

  double start_time = state->performance_last_gc_start_mark / 1e6;
  double duration = (PERFORMANCE_NOW() / 1e6) - start_time;

  std::unique_ptr<GCPerformanceEntry> entry =
      std::make_unique<GCPerformanceEntry>(
          "gc",
          start_time,
          duration,
          GCPerformanceEntry::Details(
            static_cast<PerformanceGCKind>(type),
            static_cast<PerformanceGCFlags>(flags)));

  env->SetImmediate([entry = std::move(entry)](Environment* env) {
    entry->Notify(env);
  }, CallbackFlags::kUnrefed);
}

void GarbageCollectionCleanupHook(void* data) {
  Environment* env = static_cast<Environment*>(data);
  env->isolate()->RemoveGCPrologueCallback(MarkGarbageCollectionStart, data);
  env->isolate()->RemoveGCEpilogueCallback(MarkGarbageCollectionEnd, data);
}

static void InstallGarbageCollectionTracking(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  env->isolate()->AddGCPrologueCallback(MarkGarbageCollectionStart,
                                        static_cast<void*>(env));
  env->isolate()->AddGCEpilogueCallback(MarkGarbageCollectionEnd,
                                        static_cast<void*>(env));
  env->AddCleanupHook(GarbageCollectionCleanupHook, env);
}

static void RemoveGarbageCollectionTracking(
  const FunctionCallbackInfo<Value> &args) {
  Environment* env = Environment::GetCurrent(args);

  env->RemoveCleanupHook(GarbageCollectionCleanupHook, env);
  GarbageCollectionCleanupHook(env);
}

// Gets the name of a function
inline Local<Value> GetName(Local<Function> fn) {
  Local<Value> val = fn->GetDebugName();
  if (val.IsEmpty() || val->IsUndefined()) {
    Local<Value> boundFunction = fn->GetBoundFunction();
    if (!boundFunction.IsEmpty() && !boundFunction->IsUndefined()) {
      val = GetName(boundFunction.As<Function>());
    }
  }
  return val;
}

// Notify a custom PerformanceEntry to observers
void Notify(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Utf8Value type(env->isolate(), args[0]);
  Local<Value> entry = args[1];
  PerformanceEntryType entry_type = ToPerformanceEntryTypeEnum(*type);
  AliasedUint32Array& observers = env->performance_state()->observers;
  if (entry_type != NODE_PERFORMANCE_ENTRY_TYPE_INVALID &&
      observers[entry_type]) {
    USE(env->performance_entry_callback()->
      Call(env->context(), Undefined(env->isolate()), 1, &entry));
  }
}

// Return idle time of the event loop
void LoopIdleTime(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  uint64_t idle_time = uv_metrics_idle_time(env->event_loop());
  args.GetReturnValue().Set(1.0 * idle_time / 1e6);
}

void CreateELDHistogram(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  int64_t interval = args[0].As<Integer>()->Value();
  CHECK_GT(interval, 0);
  BaseObjectPtr<IntervalHistogram> histogram =
      IntervalHistogram::Create(env, interval, [](Histogram& histogram) {
        uint64_t delta = histogram.RecordDelta();
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                        "delay", delta);
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                      "min", histogram.Min());
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                      "max", histogram.Max());
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                      "mean", histogram.Mean());
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                      "stddev", histogram.Stddev());
      }, Histogram::Options { 1000 });
  args.GetReturnValue().Set(histogram->object());
}

void GetTimeOrigin(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(Number::New(args.GetIsolate(), timeOrigin / 1e6));
}

void GetTimeOriginTimeStamp(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(
      Number::New(args.GetIsolate(), timeOriginTimestamp / MICROS_PER_MILLIS));
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();
  PerformanceState* state = env->performance_state();

  target->Set(context,
              FIXED_ONE_BYTE_STRING(isolate, "observerCounts"),
              state->observers.GetJSArray()).Check();
  target->Set(context,
              FIXED_ONE_BYTE_STRING(isolate, "milestones"),
              state->milestones.GetJSArray()).Check();

  Local<String> performanceEntryString =
      FIXED_ONE_BYTE_STRING(isolate, "PerformanceEntry");

  Local<FunctionTemplate> pe = FunctionTemplate::New(isolate);
  pe->SetClassName(performanceEntryString);
  Local<Function> fn = pe->GetFunction(context).ToLocalChecked();
  target->Set(context, performanceEntryString, fn).Check();
  env->set_performance_entry_template(fn);

  env->SetMethod(target, "markMilestone", MarkMilestone);
  env->SetMethod(target, "setupObservers", SetupPerformanceObservers);
  env->SetMethod(target,
                 "installGarbageCollectionTracking",
                 InstallGarbageCollectionTracking);
  env->SetMethod(target,
                 "removeGarbageCollectionTracking",
                 RemoveGarbageCollectionTracking);
  env->SetMethod(target, "notify", Notify);
  env->SetMethod(target, "loopIdleTime", LoopIdleTime);
  env->SetMethod(target, "getTimeOrigin", GetTimeOrigin);
  env->SetMethod(target, "getTimeOriginTimestamp", GetTimeOriginTimeStamp);
  env->SetMethod(target, "createELDHistogram", CreateELDHistogram);

  Local<Object> constants = Object::New(isolate);

  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_MAJOR);
  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_MINOR);
  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_INCREMENTAL);
  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_WEAKCB);

  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_NO);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_FORCED);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE);

#define V(name, _)                                                            \
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NODE_PERFORMANCE_ENTRY_TYPE_##name);
  NODE_PERFORMANCE_ENTRY_TYPES(V)
#undef V

#define V(name, _)                                                            \
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NODE_PERFORMANCE_MILESTONE_##name);
  NODE_PERFORMANCE_MILESTONES(V)
#undef V

  PropertyAttribute attr =
      static_cast<PropertyAttribute>(ReadOnly | DontDelete);

  target->DefineOwnProperty(context,
                            env->constants_string(),
                            constants,
                            attr).ToChecked();

  HistogramBase::Initialize(env, target);
}

void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(MarkMilestone);
  registry->Register(SetupPerformanceObservers);
  registry->Register(InstallGarbageCollectionTracking);
  registry->Register(RemoveGarbageCollectionTracking);
  registry->Register(Notify);
  registry->Register(LoopIdleTime);
  registry->Register(GetTimeOrigin);
  registry->Register(GetTimeOriginTimeStamp);
  registry->Register(CreateELDHistogram);
  HistogramBase::RegisterExternalReferences(registry);
  IntervalHistogram::RegisterExternalReferences(registry);
}
}  // namespace performance
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(performance, node::performance::Initialize)
NODE_MODULE_EXTERNAL_REFERENCE(performance,
                               node::performance::RegisterExternalReferences)
