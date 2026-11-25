#ifndef SRC_NODE_PERF_H_
#define SRC_NODE_PERF_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "base_object-inl.h"
#include "histogram.h"
#include "node.h"
#include "node_internals.h"
#include "node_perf_common.h"
#include "replayio.h"

#include "v8.h"
#include "uv.h"

#include <string>
#include <vector>

namespace node {

class Environment;
class ExternalReferenceRegistry;

namespace performance {

inline const char* GetPerformanceMilestoneName(
    PerformanceMilestone milestone) {
  switch (milestone) {
#define V(name, label) case NODE_PERFORMANCE_MILESTONE_##name: return label;
  NODE_PERFORMANCE_MILESTONES(V)
#undef V
    default:
      UNREACHABLE();
  }
}

inline PerformanceMilestone ToPerformanceMilestoneEnum(const char* str) {
#define V(name, label)                                                        \
  if (strcmp(str, label) == 0) return NODE_PERFORMANCE_MILESTONE_##name;
  NODE_PERFORMANCE_MILESTONES(V)
#undef V
  return NODE_PERFORMANCE_MILESTONE_INVALID;
}

inline const char* GetPerformanceEntryTypeName(
    PerformanceEntryType type) {
  switch (type) {
#define V(name, label) case NODE_PERFORMANCE_ENTRY_TYPE_##name: return label;
  NODE_PERFORMANCE_ENTRY_TYPES(V)
#undef V
    default:
      UNREACHABLE();
  }
}

inline PerformanceEntryType ToPerformanceEntryTypeEnum(
    const char* type) {
#define V(name, label)                                                        \
  if (strcmp(type, label) == 0) return NODE_PERFORMANCE_ENTRY_TYPE_##name;
  NODE_PERFORMANCE_ENTRY_TYPES(V)
#undef V
  return NODE_PERFORMANCE_ENTRY_TYPE_INVALID;
}

enum PerformanceGCKind {
  NODE_PERFORMANCE_GC_MAJOR = v8::GCType::kGCTypeMarkSweepCompact,
  NODE_PERFORMANCE_GC_MINOR = v8::GCType::kGCTypeScavenge,
  NODE_PERFORMANCE_GC_MINOR_MARK_SWEEP = v8::GCType::kGCTypeMinorMarkSweep,
  NODE_PERFORMANCE_GC_INCREMENTAL = v8::GCType::kGCTypeIncrementalMarking,
  NODE_PERFORMANCE_GC_WEAKCB = v8::GCType::kGCTypeProcessWeakCallbacks
};

enum PerformanceGCFlags {
  NODE_PERFORMANCE_GC_FLAGS_NO =
    v8::GCCallbackFlags::kNoGCCallbackFlags,
  NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED =
    v8::GCCallbackFlags::kGCCallbackFlagConstructRetainedObjectInfos,
  NODE_PERFORMANCE_GC_FLAGS_FORCED =
    v8::GCCallbackFlags::kGCCallbackFlagForced,
  NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING =
    v8::GCCallbackFlags::kGCCallbackFlagSynchronousPhantomCallbackProcessing,
  NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE =
    v8::GCCallbackFlags::kGCCallbackFlagCollectAllAvailableGarbage,
  NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY =
    v8::GCCallbackFlags::kGCCallbackFlagCollectAllExternalMemory,
  NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE =
    v8::GCCallbackFlags::kGCCallbackScheduleIdleGarbageCollection
};

template <typename Traits>
struct PerformanceEntry {
  using Details = typename Traits::Details;
  std::string name;
  double start_time;
  double duration;
  Details details;

  PerformanceEntry(
    const std::string& name_,
    double start_time_,
    double duration_,
    const Details& details_)
    : name(name_),
      start_time(start_time_),
      duration(duration_),
      details(details_) {}

  static v8::MaybeLocal<v8::Object> GetDetails(
      Environment* env,
      const PerformanceEntry<Traits>& entry) {
    return Traits::GetDetails(env, entry);
  }

  void Notify(Environment* env) {
    v8::HandleScope handle_scope(env->isolate());
    v8::Context::Scope scope(env->context());
    AliasedUint32Array& observers = env->performance_state()->observers;
    if (env->performance_entry_callback().IsEmpty() ||
        !observers[Traits::kType]) {
      return;
    }

    v8::Local<v8::Object> detail;
    if (!Traits::GetDetails(env, *this).ToLocal(&detail)) {
      // TODO(@jasnell): Handle the error here
      return;
    }

    v8::Local<v8::Value> argv[] = {
        OneByteString(env->isolate(), name),
        OneByteString(env->isolate(),
                      GetPerformanceEntryTypeName(Traits::kType)),
        v8::Number::New(env->isolate(), start_time),
        v8::Number::New(env->isolate(), duration),
        detail};

    node::MakeSyncCallback(
        env->isolate(),
        env->context()->Global(),
        env->performance_entry_callback(),
        arraysize(argv),
        argv);
  }
};

struct GCPerformanceEntryTraits {
  static constexpr PerformanceEntryType kType =
      NODE_PERFORMANCE_ENTRY_TYPE_GC;
  struct Details {
    PerformanceGCKind kind;
    PerformanceGCFlags flags;

    Details(PerformanceGCKind kind_, PerformanceGCFlags flags_)
        : kind(kind_), flags(flags_) {}
  };

  static v8::MaybeLocal<v8::Object> GetDetails(
      Environment* env,
      const PerformanceEntry<GCPerformanceEntryTraits>& entry);
};

using GCPerformanceEntry = PerformanceEntry<GCPerformanceEntryTraits>;

// Delivers 'gc' performance entries at a deterministic point when
// recording/replaying.
//
// Upstream creates an entry in the GC's epilogue callback and hands it to an
// unrefed immediate from there. The GC runs at points the replay does not
// reproduce, and in different numbers, so neither the entries' values nor the
// immediates they push can come from the replay's own collections: the values
// are read inside the GC, and every push changes the queue that
// Environment::CheckImmediate drains, which both sides have to agree on.
//
// Instead the epilogue only notes the entry's fields while recording, and V8's
// GC poll (deps/v8/src/replay/gc-poll.h), which handles WeakRefs,
// FinalizationRegistries and node's deferred finalization the same way, calls
// Poll() to write the notes into the recording and push the immediates from
// there. When replaying, the GC's callbacks do nothing and the recorded notes
// say which entries to deliver at the same poll.
//
// An entry therefore reaches JS a turn later than upstream delivers it, and a
// collection on a V8 task that no microtask checkpoint follows has to wait for
// the next one, so an idle event loop can hold an entry until its next turn.
// The delay is the same on both sides, and costs the entry when the process
// exits before that turn: a collection during an observer's own dispatch has
// its immediate queued by the poll after that turn's native drain, where the
// GC itself would have queued it in time. Entries from earlier turns are
// unaffected, and the two sides of a recording still agree.
//
// Switched off by the "gc-performance-entries" feature, which leaves the
// recording invalidated as before.
class GCPerformanceEntries : public v8::replayio::ReplayGCPollParticipant {
 public:
  GCPerformanceEntries(Environment* env, PerformanceState* state);
  ~GCPerformanceEntries() override;

  GCPerformanceEntries(const GCPerformanceEntries&) = delete;
  GCPerformanceEntries& operator=(const GCPerformanceEntries&) = delete;

  // Whether entries are recorded/replayed rather than the recording being
  // invalidated. False without recording/replaying, so nothing changes there.
  static bool Enabled();

  // Called by MarkGarbageCollectionEnd while recording, inside the GC: notes
  // the entry for the next poll.
  void Note(double start_time,
            double duration,
            PerformanceGCKind kind,
            PerformanceGCFlags flags);

  // Records/replays the entries noted since the last call and pushes an unrefed
  // immediate for each, as the GC's epilogue does without recording/replaying.
  // Called from the poll, and from where the GC callbacks are removed, so that
  // nothing stays noted once the poll stops running. An entry is delivered
  // either way and Notify() decides whether an observer is still there to
  // receive it, which is what upstream does with an immediate it has already
  // queued: dropping the entry here instead would lose it when a program
  // re-observes 'gc' along with another type, because that lowers the observer
  // count to zero and raises it again in the same turn.
  void Flush();

  // v8::replayio::ReplayGCPollParticipant.
  bool IsActive() const override;
  bool HasPending() const override;
  void Poll() override;

 private:
  // An entry's fields, as the recording carries them.
  struct Recorded {
    double start_time;
    double duration;
    uint32_t kind;
    uint32_t flags;
  };

  Environment* env_;
  // The environment's performance state, held directly so that IsActive() does
  // not reach for it: this is created after it and destroyed before it.
  PerformanceState* state_;
  // Whether this was added to V8's GC poll.
  bool registered_ = false;
  // While recording, the entries the recording does not describe yet. Only
  // touched on the isolate's thread: the GC's callbacks run there, and so does
  // the poll.
  std::vector<Recorded> pending_;
};

}  // namespace performance
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_PERF_H_
