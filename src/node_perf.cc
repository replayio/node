#include "node_perf.h"
#include "aliased_buffer-inl.h"
#include "env-inl.h"
#include "histogram-inl.h"
#include "memory_tracker-inl.h"
#include "node_buffer.h"
#include "node_errors.h"
#include "node_external_reference.h"
#include "node_internals.h"
#include "node_process-inl.h"
#include "util-inl.h"

#include <cinttypes>

namespace node {
namespace performance {

using v8::BigInt;
using v8::Context;
using v8::DontDelete;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::GCCallbackFlags;
using v8::GCType;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Object;
using v8::ObjectTemplate;
using v8::PropertyAttribute;
using v8::ReadOnly;
using v8::Uint32;
using v8::Value;

// Microseconds in a millisecond, as a float.
#define MICROS_PER_MILLIS 1e3
// Nanoseconds in a millisecond, as a float.
#define NANOS_PER_MILLIS 1e6

// https://w3c.github.io/hr-time/#dfn-time-origin
uint64_t performance_process_start;
// https://w3c.github.io/hr-time/#dfn-time-origin-timestamp
double performance_process_start_timestamp;
uint64_t performance_v8_start;

void InitPerformance() {
  // Don't initialize these statically so they will have consistent values when
  // recording/replaying.
  performance_process_start = PERFORMANCE_NOW();
  performance_process_start_timestamp = GetCurrentTimeInMicroseconds();
}

PerformanceState::PerformanceState(Isolate* isolate,
                                   uint64_t time_origin,
                                   double time_origin_timestamp,
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
                MAYBE_FIELD_PTR(info, observers)),
      uv_metrics(isolate,
                 offsetof(performance_state_internal, uv_metrics),
                 3,
                 root,
                 MAYBE_FIELD_PTR(info, uv_metrics)),
      uv_metrics_bigint(isolate,
                        offsetof(performance_state_internal, uv_metrics_bigint),
                        3,
                        root,
                        MAYBE_FIELD_PTR(info, uv_metrics_bigint)) {
  if (info == nullptr) {
    // For performance states initialized from scratch, reset
    // all the milestones and initialize the time origin.
    // For deserialized performance states, we will do the
    // initialization in the deserialize callback.
    ResetMilestones();
    Initialize(time_origin, time_origin_timestamp);
  }
}

void PerformanceState::ResetMilestones() {
  size_t milestones_length = milestones.Length();
  for (size_t i = 0; i < milestones_length; ++i) {
    milestones[i] = -1;
  }
}

PerformanceState::SerializeInfo PerformanceState::Serialize(
    v8::Local<v8::Context> context, v8::SnapshotCreator* creator) {
  // Reset all the milestones to improve determinism in the snapshot.
  // We'll re-initialize them after deserialization.
  ResetMilestones();

  // Do not retain runtime metrics in the snapshot.
  for (size_t i = 0; i < uv_metrics.Length(); ++i) {
    uv_metrics[i] = 0;
  }
  for (size_t i = 0; i < uv_metrics_bigint.Length(); ++i) {
    uv_metrics_bigint[i] = 0;
  }

  SerializeInfo info{root.Serialize(context, creator),
                     milestones.Serialize(context, creator),
                     observers.Serialize(context, creator),
                     uv_metrics.Serialize(context, creator),
                     uv_metrics_bigint.Serialize(context, creator)};
  return info;
}

void PerformanceState::Initialize(uint64_t time_origin,
                                  double time_origin_timestamp) {
  // We are only reusing the milestone array to store the time origin
  // and time origin timestamp, so do not use the Mark() method.
  // The time origin milestone is not exposed to user land.
  this->milestones[NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN] =
      static_cast<double>(time_origin);
  this->milestones[NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP] =
      time_origin_timestamp;
}

void PerformanceState::Deserialize(v8::Local<v8::Context> context,
                                   uint64_t time_origin,
                                   double time_origin_timestamp) {
  // Resets the pointers.
  root.Deserialize(context);
  milestones.Deserialize(context);
  observers.Deserialize(context);
  uv_metrics.Deserialize(context);
  uv_metrics_bigint.Deserialize(context);

  // Re-initialize the time origin and timestamp i.e. the process start time.
  Initialize(time_origin, time_origin_timestamp);
}

std::ostream& operator<<(std::ostream& o,
                         const PerformanceState::SerializeInfo& i) {
  o << "{\n"
    << "  " << i.root << ",  // root\n"
    << "  " << i.milestones << ",  // milestones\n"
    << "  " << i.observers << ",  // observers\n"
    << "  " << i.uv_metrics << ",  // uv_metrics\n"
    << "  " << i.uv_metrics_bigint << ",  // uv_metrics_bigint\n"
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

void SetupPerformanceObservers(const FunctionCallbackInfo<Value>& args) {
  Realm* realm = Realm::GetCurrent(args);
  // TODO(legendecas): Remove this check once the sub-realms are supported.
  CHECK_EQ(realm->kind(), Realm::Kind::kPrincipal);
  CHECK(args[0]->IsFunction());
  realm->set_performance_entry_callback(args[0].As<Function>());
}

// Marks the start of a GC cycle
void MarkGarbageCollectionStart(
    Isolate* isolate,
    GCType type,
    GCCallbackFlags flags,
    void* data) {
  Environment* env = static_cast<Environment*>(data);
  // A replay has its own collections, which describe nothing: the entries it
  // delivers come from the recording, at the GC poll. The callbacks are still
  // installed, so that what the cleanup hook removes is what was added.
  if (v8::recordreplay::IsReplaying() && GCPerformanceEntries::Enabled()) {
    return;
  }
  // Prevent gc callback from reentering with different type
  // See https://github.com/nodejs/node/issues/44046
  if (env->performance_state()->current_gc_type != 0) {
    return;
  }
  // Events are disallowed inside the GC, so the clock is read for real on both
  // sides. Only the value the recording carries is delivered, written by
  // GCPerformanceEntries::Poll at a point the replay reaches too.
  {
    v8::replayio::AutoPassThroughEvents pass_through;
    env->performance_state()->performance_last_gc_start_mark =
        PERFORMANCE_NOW();
  }
  env->performance_state()->current_gc_type = type;
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
  if (type != state->current_gc_type) {
    return;
  }
  env->performance_state()->current_gc_type = 0;
  // If no one is listening to gc performance entries, do not create them.
  if (!state->observers[NODE_PERFORMANCE_ENTRY_TYPE_GC]) [[likely]] {
    return;
  }

  double start_time =
      (state->performance_last_gc_start_mark - env->time_origin()) /
      NANOS_PER_MILLIS;
  double now;
  {
    // As in MarkGarbageCollectionStart.
    v8::replayio::AutoPassThroughEvents pass_through;
    now = PERFORMANCE_NOW();
  }
  double duration = (now / NANOS_PER_MILLIS) -
                    (state->performance_last_gc_start_mark / NANOS_PER_MILLIS);

  // Nothing is delivered from inside the GC when recording/replaying: the poll
  // does it, from the recording's own entries.
  if (GCPerformanceEntries::Enabled()) {
    env->gc_performance_entries()->Note(
        start_time,
        duration,
        static_cast<PerformanceGCKind>(type),
        static_cast<PerformanceGCFlags>(flags));
    return;
  }

  std::unique_ptr<GCPerformanceEntry> entry =
      std::make_unique<GCPerformanceEntry>(
          "gc",
          start_time,
          duration,
          GCPerformanceEntry::Details(static_cast<PerformanceGCKind>(type),
                                      static_cast<PerformanceGCFlags>(flags)));

  // The branch above returned, so this is the path of a build that is not
  // recording/replaying. A rebase which drops that branch, or its braces, would
  // deliver an entry from inside the GC, at a point the replay never reaches.
  CHECK(!GCPerformanceEntries::Enabled());
  env->SetImmediate([entry = std::move(entry)](Environment* env) {
    entry->Notify(env);
  }, CallbackFlags::kUnrefed);
}

void GarbageCollectionCleanupHook(void* data) {
  Environment* env = static_cast<Environment*>(data);
  PerformanceState* state = env->performance_state();
  if (!state->gc_tracking_installed) return;
  // Reset current_gc_type to 0
  state->current_gc_type = 0;
  env->isolate()->RemoveGCPrologueCallback(MarkGarbageCollectionStart, data);
  env->isolate()->RemoveGCEpilogueCallback(MarkGarbageCollectionEnd, data);
  state->gc_tracking_installed = false;
}

// Registers the GC callbacks with V8 if and only if GC timing is needed,
// i.e. there are 'gc' PerformanceObservers. This is idempotent, so it never
// adds the callbacks twice or removes callbacks that are not registered.
static void ReconcileGarbageCollectionTracking(Environment* env) {
  PerformanceState* state = env->performance_state();
  const bool wanted = state->observers[NODE_PERFORMANCE_ENTRY_TYPE_GC] > 0;
  if (wanted == state->gc_tracking_installed) return;

  if (wanted) {
    // GC performance entries are created at points which differ between
    // recording and replaying, and are delivered to JS. GCPerformanceEntries
    // records the ones the recording's collections produced and delivers those
    // at the GC poll; without it they are not supported when
    // recording/replaying, as other performance entries are not. Only
    // installing the callbacks observes them: a call that removes them, or one
    // that restores the state of a snapshot built without them, does not.
    if (!GCPerformanceEntries::Enabled()) {
      v8::recordreplay::InvalidateRecording("GC performance entries observed");
    }

    // Reset current_gc_type to 0
    state->current_gc_type = 0;
    env->isolate()->AddGCPrologueCallback(MarkGarbageCollectionStart,
                                          static_cast<void*>(env));
    env->isolate()->AddGCEpilogueCallback(MarkGarbageCollectionEnd,
                                          static_cast<void*>(env));
    env->AddCleanupHook(GarbageCollectionCleanupHook, env);
    state->gc_tracking_installed = true;
  } else {
    if (GCPerformanceEntries::Enabled()) {
      env->gc_performance_entries()->Flush();
    }
    env->RemoveCleanupHook(GarbageCollectionCleanupHook, env);
    GarbageCollectionCleanupHook(env);
  }
}

static void UpdateGarbageCollectionTracking(
    const FunctionCallbackInfo<Value>& args) {
  ReconcileGarbageCollectionTracking(Environment::GetCurrent(args));
}

GCPerformanceEntries::GCPerformanceEntries(Environment* env,
                                           PerformanceState* state)
    : env_(env), state_(state) {
  if (!Enabled()) return;
  registered_ = true;
  v8::replayio::AddReplayGCPollParticipant(env_->isolate(), this);
}

GCPerformanceEntries::~GCPerformanceEntries() {
  if (!registered_) return;
  v8::replayio::RemoveReplayGCPollParticipant(env_->isolate(), this);
}

bool GCPerformanceEntries::Enabled() {
  return v8::recordreplay::IsRecordingOrReplaying("gc-performance-entries");
}

void GCPerformanceEntries::Note(double start_time,
                                double duration,
                                PerformanceGCKind kind,
                                PerformanceGCFlags flags) {
  // A replay's own collections describe nothing, and its callbacks return
  // before this.
  CHECK(v8::recordreplay::IsRecording());
  pending_.push_back(Recorded{start_time,
                              duration,
                              static_cast<uint32_t>(kind),
                              static_cast<uint32_t>(flags)});
}

bool GCPerformanceEntries::IsActive() const {
  // The same on both sides, as the poll requires: the flag is only changed by
  // ReconcileGarbageCollectionTracking, which runs where the observer count
  // changes, and no poll can run in between.
  return registered_ && state_->gc_tracking_installed;
}

bool GCPerformanceEntries::HasPending() const {
  return !pending_.empty();
}

void GCPerformanceEntries::Poll() {
  Flush();
}

void GCPerformanceEntries::Flush() {
  std::vector<Recorded> entries;
  entries.swap(pending_);
  entries.resize(v8::recordreplay::RecordReplayValue(
      "GCPerformanceEntries.flush", entries.size()));
  if (entries.empty()) return;
  v8::recordreplay::RecordReplayBytes("GCPerformanceEntries.flush entries",
                                      entries.data(),
                                      entries.size() * sizeof(Recorded));

  // Delivered the way the GC's epilogue delivers them without
  // recording/replaying: one unrefed immediate each, which keeps the entries
  // out of the poll, out of JS, and off the list of things that hold the event
  // loop open. Observers see them sorted by startTime either way, so the order
  // they are pushed in is not what a program reads.
  for (const Recorded& entry : entries) {
    auto delivered = std::make_unique<GCPerformanceEntry>(
        "gc",
        entry.start_time,
        entry.duration,
        GCPerformanceEntry::Details(
            static_cast<PerformanceGCKind>(entry.kind),
            static_cast<PerformanceGCFlags>(entry.flags)));
    env_->SetImmediate(
        [delivered = std::move(delivered)](Environment* env) {
          delivered->Notify(env);
        },
        CallbackFlags::kUnrefed);
  }
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
    // Performance entries can be non-deterministic and are not currently
    // supported when recording/replaying.
    v8::recordreplay::InvalidateRecording("Performance entries observed");
    USE(env->performance_entry_callback()->
      Call(env->context(), Undefined(env->isolate()), 1, &entry));
  }
}

// Return idle time of the event loop
void LoopIdleTime(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  uint64_t idle_time = uv_metrics_idle_time(env->event_loop());
  args.GetReturnValue().Set(1.0 * idle_time / NANOS_PER_MILLIS);
}

void UvMetricsInfo(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  uv_metrics_t metrics;
  // uv_metrics_info always return 0
  CHECK_EQ(uv_metrics_info(env->event_loop(), &metrics), 0);
  // libuv reports 64-bit counters. The doubles backing uvMetricsInfo are
  // exact up to Number.MAX_SAFE_INTEGER, while the uint64_t values backing
  // uvMetricsInfoBigInt carry the full range.
  PerformanceState* state = env->performance_state();
  const uint64_t values[] = {
      metrics.loop_count, metrics.events, metrics.events_waiting};
  for (size_t i = 0; i < arraysize(values); ++i) {
    state->uv_metrics[i] = static_cast<double>(values[i]);
    state->uv_metrics_bigint[i] = values[i];
  }
}

void CreateELDHistogram(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  int64_t interval = args[0].As<Integer>()->Value();
  CHECK_GT(interval, 0);
  CHECK(args[2]->IsBigInt());
  CHECK(args[3]->IsBigInt());
  CHECK(args[4]->IsUint32());
  bool lossless = true;
  const int64_t lowest = args[2].As<BigInt>()->Int64Value(&lossless);
  CHECK(lossless);
  const int64_t highest = args[3].As<BigInt>()->Int64Value(&lossless);
  CHECK(lossless);
  const int figures = static_cast<int>(args[4].As<Uint32>()->Value());

  // The options are validated in JS, but hdr_init() still rejects some
  // combinations, such as a very large lowest value.
  std::shared_ptr<Histogram> histogram =
      Histogram::Create(Histogram::Options{lowest, highest, figures});
  if (!histogram) {
    return THROW_ERR_INVALID_ARG_VALUE(env, "Invalid histogram options");
  }

  if (args[1]->IsTrue()) {
    BaseObjectPtr<IterationHistogram> eld =
        IterationHistogram::Create(env, std::move(histogram));
    if (eld) args.GetReturnValue().Set(eld->object());
    return;
  }
  BaseObjectPtr<IntervalHistogram> eld = IntervalHistogram::Create(
      env,
      interval,
      [](Histogram& histogram) {
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
      },
      std::move(histogram));
  if (eld) args.GetReturnValue().Set(eld->object());
}

void MarkBootstrapComplete(const FunctionCallbackInfo<Value>& args) {
  Realm* realm = Realm::GetCurrent(args);
  CHECK_EQ(realm->kind(), Realm::Kind::kPrincipal);
  realm->env()->performance_state()->Mark(
      performance::NODE_PERFORMANCE_MILESTONE_BOOTSTRAP_COMPLETE);
}

static double PerformanceNowImpl() {
  return static_cast<double>(uv_hrtime() - performance_process_start) /
         NANOS_PER_MILLIS;
}

static double FastPerformanceNow(v8::Local<v8::Value> receiver) {
  return PerformanceNowImpl();
}

static void SlowPerformanceNow(const FunctionCallbackInfo<Value>& args) {
  args.GetReturnValue().Set(PerformanceNowImpl());
}

static v8::CFunction fast_performance_now(
    v8::CFunction::Make(FastPerformanceNow));

static void CreatePerIsolateProperties(IsolateData* isolate_data,
                                       Local<ObjectTemplate> target) {
  Isolate* isolate = isolate_data->isolate();

  HistogramBase::Initialize(isolate_data, target);
  SlidingWindowHistogram::Initialize(isolate_data, target);

  SetMethod(isolate, target, "setupObservers", SetupPerformanceObservers);
  SetMethod(isolate,
            target,
            "updateGarbageCollectionTracking",
            UpdateGarbageCollectionTracking);
  SetMethod(isolate, target, "notify", Notify);
  SetMethod(isolate, target, "loopIdleTime", LoopIdleTime);
  SetMethod(isolate, target, "createELDHistogram", CreateELDHistogram);
  SetMethod(isolate, target, "markBootstrapComplete", MarkBootstrapComplete);
  SetMethod(isolate, target, "uvMetricsInfo", UvMetricsInfo);
  SetFastMethodNoSideEffect(
      isolate, target, "now", SlowPerformanceNow, &fast_performance_now);
}

void CreatePerContextProperties(Local<Object> target,
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
  target
      ->Set(context,
            FIXED_ONE_BYTE_STRING(isolate, "uvMetricsBuffer"),
            state->uv_metrics.GetJSArray())
      .Check();
  target
      ->Set(context,
            FIXED_ONE_BYTE_STRING(isolate, "uvMetricsBigIntBuffer"),
            state->uv_metrics_bigint.GetJSArray())
      .Check();

  Local<Object> constants = Object::New(isolate);

  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_MAJOR);
  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_MINOR);
  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_MINOR_MARK_SWEEP);
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

  target->DefineOwnProperty(context, env->constants_string(), constants, attr)
      .ToChecked();
}

void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(SetupPerformanceObservers);
  registry->Register(UpdateGarbageCollectionTracking);
  registry->Register(Notify);
  registry->Register(LoopIdleTime);
  registry->Register(CreateELDHistogram);
  registry->Register(MarkBootstrapComplete);
  registry->Register(UvMetricsInfo);
  registry->Register(SlowPerformanceNow);
  registry->Register(fast_performance_now);
  HistogramBase::RegisterExternalReferences(registry);
  SlidingWindowHistogram::RegisterExternalReferences(registry);
  IntervalHistogram::RegisterExternalReferences(registry);
  IterationHistogram::RegisterExternalReferences(registry);
}
}  // namespace performance
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(
    performance, node::performance::CreatePerContextProperties)
NODE_BINDING_PER_ISOLATE_INIT(performance,
                              node::performance::CreatePerIsolateProperties)
NODE_BINDING_EXTERNAL_REFERENCE(performance,
                                node::performance::RegisterExternalReferences)
