// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/memory-reducer.h"

#include <cmath>

#include "src/flags/flags.h"
#include "src/heap/gc-tracer.h"
#include "src/heap/heap-inl.h"
#include "src/heap/incremental-marking.h"
#include "src/init/v8.h"
#include "src/utils/utils.h"

namespace v8 {
namespace internal {

const int MemoryReducer::kLongDelayMs = 8000;
const int MemoryReducer::kShortDelayMs = 500;
const int MemoryReducer::kWatchdogDelayMs = 100000;
const int MemoryReducer::kMaxNumberOfGCs = 3;
const double MemoryReducer::kCommittedMemoryFactor = 1.1;
const size_t MemoryReducer::kCommittedMemoryDelta = 10 * MB;

MemoryReducer::MemoryReducer(Heap* heap)
    : heap_(heap),
      taskrunner_(V8::GetCurrentPlatform()->GetForegroundTaskRunner(
          reinterpret_cast<v8::Isolate*>(heap->isolate()))),
      state_(kDone, 0, 0.0, 0.0, 0),
      js_calls_counter_(0),
      js_calls_sample_time_ms_(0.0) {}

MemoryReducer::TimerTask::TimerTask(MemoryReducer* memory_reducer)
    : CancelableTask(memory_reducer->heap()->isolate()),
      memory_reducer_(memory_reducer) {}


void MemoryReducer::TimerTask::RunInternal() {
  // When recording/replaying, the timer runs at the same point in both, but
  // the memory reducer's state and what it decides depend on the GC's history,
  // which differs between them. Like the GC, it runs with events disallowed,
  // and posts the next timer afterwards if the recording requested one.
  memory_reducer_->record_replay_timer_posted_ = false;
  {
    replayio::AutoDisallowEvents disallow("MemoryReducer::TimerTask::RunInternal");
    if (!recordreplay::IsRecordingOrReplaying() ||
        memory_reducer_->state_.action == kWait) {
      RunTimer();
    }
  }
  if (recordreplay::IsRecordingOrReplaying()) {
    memory_reducer_->RecordReplayPostRequestedTimer();
  }
}

void MemoryReducer::TimerTask::RunTimer() {
  Heap* heap = memory_reducer_->heap();
  Event event;
  double time_ms = heap->MonotonicallyIncreasingTimeInMs();
  heap->tracer()->SampleAllocation(time_ms, heap->NewSpaceAllocationCounter(),
                                   heap->OldGenerationAllocationCounter(),
                                   heap->EmbedderAllocationCounter());
  bool low_allocation_rate = heap->HasLowAllocationRate();
  bool optimize_for_memory = heap->ShouldOptimizeForMemoryUsage();
  if (FLAG_trace_gc_verbose) {
    heap->isolate()->PrintWithTimestamp(
        "Memory reducer: %s, %s\n",
        low_allocation_rate ? "low alloc" : "high alloc",
        optimize_for_memory ? "background" : "foreground");
  }
  event.type = kTimer;
  event.time_ms = time_ms;
  // The memory reducer will start incremental markig if
  // 1) mutator is likely idle: js call rate is low and allocation rate is low.
  // 2) mutator is in background: optimize for memory flag is set.
  event.should_start_incremental_gc =
      low_allocation_rate || optimize_for_memory;
  event.can_start_incremental_gc =
      heap->incremental_marking()->IsStopped() &&
      (heap->incremental_marking()->CanBeActivated() || optimize_for_memory);
  event.committed_memory = heap->CommittedOldGenerationMemory();
  memory_reducer_->NotifyTimer(event);
}


void MemoryReducer::NotifyTimer(const Event& event) {
  DCHECK_EQ(kTimer, event.type);
  DCHECK_EQ(kWait, state_.action);
  state_ = Step(state_, event);
  if (state_.action == kRun) {
    DCHECK(heap()->incremental_marking()->IsStopped());
    DCHECK(FLAG_incremental_marking);
    if (FLAG_trace_gc_verbose) {
      heap()->isolate()->PrintWithTimestamp("Memory reducer: started GC #%d\n",
                                            state_.started_gcs);
    }
    if (recordreplay::IsRecordingOrReplaying()) {
      // Incremental marking is driven by tasks, which node's platform only
      // runs at the next event loop iteration when recording/replaying, so
      // on an idle program the GC wouldn't finish. Run it to completion
      // instead.
      heap()->CollectAllGarbage(Heap::kReduceMemoryFootprintMask,
                                GarbageCollectionReason::kMemoryReducer,
                                kGCCallbackFlagCollectAllExternalMemory);
    } else {
      heap()->StartIdleIncrementalMarking(
          GarbageCollectionReason::kMemoryReducer,
          kGCCallbackFlagCollectAllExternalMemory);
    }
  } else if (state_.action == kWait) {
    if (!heap()->incremental_marking()->IsStopped() &&
        heap()->ShouldOptimizeForMemoryUsage()) {
      // Make progress with pending incremental marking if memory usage has
      // higher priority than latency. This is important for background tabs
      // that do not send idle notifications.
      const int kIncrementalMarkingDelayMs = 500;
      double deadline = heap()->MonotonicallyIncreasingTimeInMs() +
                        kIncrementalMarkingDelayMs;
      heap()->incremental_marking()->AdvanceWithDeadline(
          deadline, IncrementalMarking::NO_GC_VIA_STACK_GUARD,
          StepOrigin::kTask);
      heap()->FinalizeIncrementalMarkingIfComplete(
          GarbageCollectionReason::kFinalizeMarkingViaTask);
    }
    // Re-schedule the timer.
    ScheduleTimer(state_.next_gc_start_ms - event.time_ms);
    if (FLAG_trace_gc_verbose) {
      heap()->isolate()->PrintWithTimestamp(
          "Memory reducer: waiting for %.f ms\n",
          state_.next_gc_start_ms - event.time_ms);
    }
  }
}


void MemoryReducer::NotifyMarkCompact(const Event& event) {
  DCHECK_EQ(kMarkCompact, event.type);
  Action old_action = state_.action;
  state_ = Step(state_, event);
  if (old_action != kWait && state_.action == kWait) {
    // If we are transitioning to the WAIT state, start the timer.
    ScheduleTimer(state_.next_gc_start_ms - event.time_ms);
  }
  if (old_action == kRun) {
    if (FLAG_trace_gc_verbose) {
      heap()->isolate()->PrintWithTimestamp(
          "Memory reducer: finished GC #%d (%s)\n", state_.started_gcs,
          state_.action == kWait ? "will do more" : "done");
    }
  }
}

void MemoryReducer::NotifyPossibleGarbage(const Event& event) {
  DCHECK_EQ(kPossibleGarbage, event.type);
  Action old_action = state_.action;
  state_ = Step(state_, event);
  if (old_action != kWait && state_.action == kWait) {
    // If we are transitioning to the WAIT state, start the timer.
    ScheduleTimer(state_.next_gc_start_ms - event.time_ms);
  }
}


bool MemoryReducer::WatchdogGC(const State& state, const Event& event) {
  return state.last_gc_time_ms != 0 &&
         event.time_ms > state.last_gc_time_ms + kWatchdogDelayMs;
}


// For specification of this function see the comment for MemoryReducer class.
MemoryReducer::State MemoryReducer::Step(const State& state,
                                         const Event& event) {
  if (!FLAG_incremental_marking || !FLAG_memory_reducer) {
    return State(kDone, 0, 0, state.last_gc_time_ms, 0);
  }
  switch (state.action) {
    case kDone:
      if (event.type == kTimer) {
        return state;
      } else if (event.type == kMarkCompact) {
        if (event.committed_memory <
            std::max(
                static_cast<size_t>(state.committed_memory_at_last_run *
                                    kCommittedMemoryFactor),
                state.committed_memory_at_last_run + kCommittedMemoryDelta)) {
          return state;
        } else {
          return State(kWait, 0, event.time_ms + kLongDelayMs,
                       event.type == kMarkCompact ? event.time_ms
                                                  : state.last_gc_time_ms,
                       0);
        }
      } else {
        DCHECK_EQ(kPossibleGarbage, event.type);
        return State(
            kWait, 0, event.time_ms + kLongDelayMs,
            event.type == kMarkCompact ? event.time_ms : state.last_gc_time_ms,
            0);
      }
    case kWait:
      switch (event.type) {
        case kPossibleGarbage:
          return state;
        case kTimer:
          if (state.started_gcs >= kMaxNumberOfGCs) {
            return State(kDone, kMaxNumberOfGCs, 0.0, state.last_gc_time_ms,
                         event.committed_memory);
          } else if (event.can_start_incremental_gc &&
                     (event.should_start_incremental_gc ||
                      WatchdogGC(state, event))) {
            if (state.next_gc_start_ms <= event.time_ms) {
              return State(kRun, state.started_gcs + 1, 0.0,
                           state.last_gc_time_ms, 0);
            } else {
              return state;
            }
          } else {
            return State(kWait, state.started_gcs, event.time_ms + kLongDelayMs,
                         state.last_gc_time_ms, 0);
          }
        case kMarkCompact:
          return State(kWait, state.started_gcs, event.time_ms + kLongDelayMs,
                       event.time_ms, 0);
      }
    case kRun:
      if (event.type != kMarkCompact) {
        return state;
      } else {
        if (state.started_gcs < kMaxNumberOfGCs &&
            (event.next_gc_likely_to_collect_more || state.started_gcs == 1)) {
          return State(kWait, state.started_gcs, event.time_ms + kShortDelayMs,
                       event.time_ms, 0);
        } else {
          return State(kDone, kMaxNumberOfGCs, 0.0, event.time_ms,
                       event.committed_memory);
        }
      }
  }
  UNREACHABLE();
}

extern void RecordReplayTriggerProgressInterrupt();

void MemoryReducer::ScheduleTimer(double delay_ms) {
  DCHECK_LT(0, delay_ms);
  if (heap()->IsTearingDown()) return;
  if (recordreplay::IsRecordingOrReplaying()) {
    RecordReplayRequestTimer(delay_ms);
    return;
  }
  PostTimer(delay_ms);
}

void MemoryReducer::PostTimer(double delay_ms) {
  // Leave some room for precision error in task scheduler.
  const double kSlackMs = 100;
  taskrunner_->PostDelayedTask(std::make_unique<MemoryReducer::TimerTask>(this),
                               (delay_ms + kSlackMs) / 1000.0);
}

// When recording/replaying, whether the timer is posted depends on the GC's
// history, which differs between recording and replaying, while posting it
// is a recorded event, and the GC often requests it with events disallowed.
// A request is remembered and RecordReplayPostRequestedTimer posts the timer
// at the next point which happens at the same time when replaying as when
// recording, if the recording had a request and no posted timer is pending:
// after the timer task, after a context disposal, or at a progress interrupt,
// which a request triggers when recording. Only the main thread has progress
// interrupts; memory reducers on other threads don't run.
void MemoryReducer::RecordReplayRequestTimer(double delay_ms) {
  if (!IsMainThread()) return;
  record_replay_timer_delay_ms_ = delay_ms;
  if (recordreplay::IsRecording()) {
    RecordReplayTriggerProgressInterrupt();
  }
}

void MemoryReducer::RecordReplayOnProgressInterrupt() {
  RecordReplayPostRequestedTimer();
}

void MemoryReducer::RecordReplayPostRequestedTimer() {
  if (!IsMainThread() || recordreplay::AreEventsDisallowed() ||
      record_replay_timer_posted_) {
    return;
  }
  uintptr_t delay_ms = recordreplay::RecordReplayValue(
      "MemoryReducer::RecordReplayPostRequestedTimer",
      static_cast<uintptr_t>(std::ceil(record_replay_timer_delay_ms_)));
  record_replay_timer_delay_ms_ = 0;
  if (delay_ms) {
    PostTimer(static_cast<double>(delay_ms));
    record_replay_timer_posted_ = true;
  }
}

void MemoryReducer::TearDown() { state_ = State(kDone, 0, 0, 0.0, 0); }

}  // namespace internal
}  // namespace v8
