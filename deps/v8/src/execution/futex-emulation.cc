// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/futex-emulation.h"

#include <limits>
#include <memory>
#include <vector>

#include "include/replayio.h"
#include "src/api/api-inl.h"
#include "src/base/lazy-instance.h"
#include "src/base/logging.h"
#include "src/base/macros.h"
#include "src/base/optional.h"
#include "src/base/small-map.h"
#include "src/execution/isolate.h"
#include "src/execution/vm-state-inl.h"
#include "src/handles/handles-inl.h"
#include "src/numbers/conversions.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/objects-inl.h"
#include "src/tasks/cancelable-task.h"

namespace v8::internal {

using AtomicsWaitEvent = v8::Isolate::AtomicsWaitEvent;

// A {FutexWaitList} manages all contexts waiting (synchronously or
// asynchronously) on any address.
class FutexWaitList {
 public:
  FutexWaitList() = default;
  FutexWaitList(const FutexWaitList&) = delete;
  FutexWaitList& operator=(const FutexWaitList&) = delete;

  void AddNode(FutexWaitListNode* node);
  void RemoveNode(FutexWaitListNode* node);

  static void* ToWaitLocation(Tagged<JSArrayBuffer> array_buffer, size_t addr) {
    DCHECK_LT(addr, array_buffer->GetByteLength());
    // Use the cheaper JSArrayBuffer::backing_store() accessor, but DCHECK that
    // it matches the start of the JSArrayBuffer::GetBackingStore().
    DCHECK_EQ(array_buffer->backing_store(),
              array_buffer->GetBackingStore()->buffer_start());
    return static_cast<uint8_t*>(array_buffer->backing_store()) + addr;
  }

  // Deletes "node" and returns the next node of its list.
  static FutexWaitListNode* DeleteAsyncWaiterNode(FutexWaitListNode* node) {
    DCHECK(node->IsAsync());
    DCHECK_NOT_NULL(node->async_state_->isolate_for_async_waiters);
    FutexWaitListNode* next = node->next_;
    if (node->prev_ != nullptr) {
      node->prev_->next_ = next;
    }
    if (next != nullptr) {
      next->prev_ = node->prev_;
    }
    delete node;
    return next;
  }

  static void DeleteNodesForIsolate(Isolate* isolate, FutexWaitListNode** head,
                                    FutexWaitListNode** tail) {
    // For updating head & tail once we've iterated all nodes.
    FutexWaitListNode* new_head = nullptr;
    FutexWaitListNode* new_tail = nullptr;
    for (FutexWaitListNode* node = *head; node;) {
      if (node->IsAsync() &&
          node->async_state_->isolate_for_async_waiters == isolate) {
        node->async_state_->timeout_task_id =
            CancelableTaskManager::kInvalidTaskId;
        node = DeleteAsyncWaiterNode(node);
      } else {
        if (new_head == nullptr) {
          new_head = node;
        }
        new_tail = node;
        node = node->next_;
      }
    }
    *head = new_head;
    *tail = new_tail;
  }

  // For checking the internal consistency of the FutexWaitList.
  void Verify() const;
  // Returns true if |node| is on the linked list starting with |head|.
  static bool NodeIsOnList(FutexWaitListNode* node, FutexWaitListNode* head);

  base::Mutex* mutex() { return &mutex_; }

 private:
  friend class FutexEmulation;

  struct HeadAndTail {
    FutexWaitListNode* head;
    FutexWaitListNode* tail;
  };

  // Takes the nodes given the resolve id off the isolate's list of async
  // waiters to resolve, and returns them as a list of their own.
  static FutexWaitListNode* RecordReplayTakeNodesToResolve(
      HeadAndTail* list, uint64_t record_replay_resolve_id) {
    FutexWaitListNode* taken_head = nullptr;
    FutexWaitListNode* taken_tail = nullptr;
    FutexWaitListNode* node = list->head;
    while (node) {
      FutexWaitListNode* next = node->next_;
      if (node->record_replay_resolve_id_ == record_replay_resolve_id) {
        if (node->prev_) {
          node->prev_->next_ = next;
        } else {
          list->head = next;
        }
        if (next) {
          next->prev_ = node->prev_;
        } else {
          list->tail = node->prev_;
        }
        node->prev_ = taken_tail;
        node->next_ = nullptr;
        if (taken_tail) {
          taken_tail->next_ = node;
        } else {
          taken_head = node;
        }
        taken_tail = node;
      }
      node = next;
    }
    return taken_head;
  }

  // `mutex` protects the composition of the fields below (i.e. no elements may
  // be added or removed without holding this mutex), as well as the `waiting_`
  // and `interrupted_` fields for each individual list node that is currently
  // part of the list. It must be the mutex used together with the `cond_`
  // condition variable of such nodes.
  base::Mutex mutex_;

  // Location inside a shared buffer -> linked list of Nodes waiting on that
  // location.
  // As long as the map does not grow beyond 16 entries, there is no dynamic
  // allocation and deallocation happening in wait or wake, which reduces the
  // time spend in the critical section.
  base::SmallMap<std::map<void*, HeadAndTail>, 16> location_lists_;

  // Isolate* -> linked list of Nodes which are waiting for their Promises to
  // be resolved.
  base::SmallMap<std::map<Isolate*, HeadAndTail>> isolate_promises_to_resolve_;

  // The last record_replay_resolve_id_ given to notified async waiters.
  uint64_t record_replay_last_resolve_id_ = 0;
};

namespace {
// While recording or replaying, orders Wake against waiters entering and
// leaving the wait list and going away with their isolate: all of them change
// the list under the wait list's mutex with events disallowed, so the replay
// would otherwise pick its own order, and a notify would wake other waiters,
// or none.
int gRecordReplayOrderedLockId = 0;

// While recording or replaying, whether a wait found another value than the
// one it waits for is recorded: the program's stores to the memory aren't, so
// the replay could otherwise find another value than the recording did, and
// wait where it didn't or the other way around. Called with the wait list's
// mutex held and events disallowed under the given label; they are allowed
// again while the answer is recorded.
bool RecordReplayFoundOtherValue(bool found_other_value, const char* label) {
  if (!recordreplay::IsRecordingOrReplaying()) {
    return found_other_value;
  }
  recordreplay::EndDisallowEvents();
  found_other_value = recordreplay::RecordReplayValue(
      "FutexEmulation found another value", found_other_value);
  recordreplay::BeginDisallowEventsWithLabel(label);
  return found_other_value;
}

// {GetWaitList} returns the lazily initialized global wait list.
DEFINE_LAZY_LEAKY_OBJECT_GETTER(FutexWaitList, GetWaitList)

}  // namespace

void FutexEmulation::RecordReplayInitialize() {
  gRecordReplayOrderedLockId =
      (int)recordreplay::CreateOrderedLock("FutexEmulation");
}

bool FutexWaitListNode::CancelTimeoutTask() {
  DCHECK(IsAsync());
  if (async_state_->timeout_task_id == CancelableTaskManager::kInvalidTaskId) {
    return true;
  }
  auto* cancelable_task_manager =
      async_state_->isolate_for_async_waiters->cancelable_task_manager();
  TryAbortResult return_value =
      cancelable_task_manager->TryAbort(async_state_->timeout_task_id);
  async_state_->timeout_task_id = CancelableTaskManager::kInvalidTaskId;
  return return_value != TryAbortResult::kTaskRunning;
}

void FutexWaitListNode::NotifyWake() {
  DCHECK(!IsAsync());

  replayio::AutoDisallowEvents disallow("FutexWaitListNode::NotifyWake");

  // Lock the FutexEmulation mutex before notifying. We know that the mutex
  // will have been unlocked if we are currently waiting on the condition
  // variable. The mutex will not be locked if FutexEmulation::Wait hasn't
  // locked it yet. In that case, we set the interrupted_
  // flag to true, which will be tested after the mutex locked by a future wait.
  FutexWaitList* wait_list = GetWaitList();
  NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

  // if not waiting, this will not have any effect.
  cond_.NotifyOne();
  interrupted_ = true;
}

class ResolveAsyncWaiterPromisesTask : public CancelableTask {
 public:
  ResolveAsyncWaiterPromisesTask(Isolate* isolate,
                                 uint64_t record_replay_resolve_id = 0)
      : CancelableTask(isolate),
        isolate_(isolate),
        record_replay_resolve_id_(record_replay_resolve_id) {}

  void RunInternal() override {
    FutexEmulation::ResolveAsyncWaiterPromises(isolate_,
                                               record_replay_resolve_id_);
  }

 private:
  Isolate* isolate_;
  uint64_t record_replay_resolve_id_;
};

class AsyncWaiterTimeoutTask : public CancelableTask {
 public:
  AsyncWaiterTimeoutTask(CancelableTaskManager* cancelable_task_manager,
                         FutexWaitListNode* node)
      : CancelableTask(cancelable_task_manager), node_(node) {}

  void RunInternal() override {
    FutexEmulation::HandleAsyncWaiterTimeout(node_);
  }

 private:
  FutexWaitListNode* node_;
};

// While recording or replaying, FutexEmulation doesn't post tasks from inside
// its replayio::AutoDisallowEvents scopes: node's platform posts through a task
// queue mutex and uv_async_send, which are the events that order the receiving
// thread after the post, and asserts in PostTask. The tasks are queued here
// instead, and posted when this goes out of scope; declared before the scope
// and the wait list's lock, that is after both have ended.
class FutexEmulation::RecordReplayTasksToPost {
 public:
  RecordReplayTasksToPost() = default;
  RecordReplayTasksToPost(const RecordReplayTasksToPost&) = delete;
  RecordReplayTasksToPost& operator=(const RecordReplayTasksToPost&) = delete;

  ~RecordReplayTasksToPost() {
    for (Task& task : tasks_) {
      if (task.delay_in_seconds) {
        task.runner->PostNonNestableDelayedTask(std::move(task.task),
                                                task.delay_in_seconds);
      } else {
        task.runner->PostNonNestableTask(std::move(task.task));
      }
    }
  }

  void Add(const std::shared_ptr<TaskRunner>& runner,
           std::unique_ptr<CancelableTask> task, double delay_in_seconds = 0) {
    tasks_.push_back({runner, std::move(task), delay_in_seconds});
  }

  // Returns the id of this scope's task resolving async waiters' promises in
  // the node's isolate, adding the task for the first such node. Upstream
  // instead adds a notified waiter to the isolate's pending task if there is
  // one, and whether there is depends on how far the waiter's thread has got,
  // which the replay doesn't reproduce. Requires the wait list's mutex.
  uint64_t ResolveId(FutexWaitListNode* node) {
    Isolate* isolate = node->async_state_->isolate_for_async_waiters;
    for (ResolveTask& resolve_task : resolve_tasks_) {
      if (resolve_task.isolate == isolate) {
        return resolve_task.id;
      }
    }
    uint64_t id = ++GetWaitList()->record_replay_last_resolve_id_;
    resolve_tasks_.push_back({isolate, id});
    Add(node->async_state_->task_runner,
        std::make_unique<ResolveAsyncWaiterPromisesTask>(isolate, id));
    return id;
  }

 private:
  struct Task {
    std::shared_ptr<TaskRunner> runner;
    std::unique_ptr<CancelableTask> task;
    // Zero posts it as an immediate task.
    double delay_in_seconds;
  };
  struct ResolveTask {
    Isolate* isolate;
    uint64_t id;
  };
  std::vector<Task> tasks_;
  std::vector<ResolveTask> resolve_tasks_;
};

void FutexEmulation::NotifyAsyncWaiter(
    FutexWaitListNode* node, RecordReplayTasksToPost* record_replay_tasks) {
  DCHECK(node->IsAsync());
  // This function can run in any thread.

  FutexWaitList* wait_list = GetWaitList();
  wait_list->mutex()->AssertHeld();

  // Nullify the timeout time; this distinguishes timed out waiters from
  // woken up ones.
  node->async_state_->timeout_time = base::TimeTicks();

  wait_list->RemoveNode(node);

  // Schedule a task for resolving the Promise. It's still possible that the
  // timeout task runs before the promise resolving task. In that case, the
  // timeout task will just ignore the node.
  auto& isolate_map = wait_list->isolate_promises_to_resolve_;
  auto it = isolate_map.find(node->async_state_->isolate_for_async_waiters);
  if (it == isolate_map.end()) {
    // This Isolate doesn't have other Promises to resolve at the moment.
    isolate_map.insert(
        std::make_pair(node->async_state_->isolate_for_async_waiters,
                       FutexWaitList::HeadAndTail{node, node}));
    if (!recordreplay::IsRecordingOrReplaying()) {
      auto task = std::make_unique<ResolveAsyncWaiterPromisesTask>(
          node->async_state_->isolate_for_async_waiters);
      node->async_state_->task_runner->PostNonNestableTask(std::move(task));
    }
  } else {
    // Add this Node into the existing list.
    node->prev_ = it->second.tail;
    it->second.tail->next_ = node;
    it->second.tail = node;
  }
  if (recordreplay::IsRecordingOrReplaying()) {
    node->record_replay_resolve_id_ = record_replay_tasks->ResolveId(node);
  }
}

void FutexWaitList::AddNode(FutexWaitListNode* node) {
  DCHECK_NULL(node->prev_);
  DCHECK_NULL(node->next_);
  auto [it, inserted] =
      location_lists_.insert({node->wait_location_, HeadAndTail{node, node}});
  if (!inserted) {
    it->second.tail->next_ = node;
    node->prev_ = it->second.tail;
    it->second.tail = node;
  }

  Verify();
}

void FutexWaitList::RemoveNode(FutexWaitListNode* node) {
  if (!node->prev_ && !node->next_) {
    // If the node was the last one on its list, delete the whole list.
    size_t erased = location_lists_.erase(node->wait_location_);
    DCHECK_EQ(1, erased);
    USE(erased);
  } else if (node->prev_ && node->next_) {
    // If we have both a successor and a predecessor, skip the lookup in the
    // list and just update those two nodes directly.
    node->prev_->next_ = node->next_;
    node->next_->prev_ = node->prev_;
    node->prev_ = node->next_ = nullptr;
  } else {
    // Otherwise we have to lookup in the list to find the head and tail
    // pointers.
    auto it = location_lists_.find(node->wait_location_);
    DCHECK_NE(location_lists_.end(), it);
    DCHECK(NodeIsOnList(node, it->second.head));

    if (node->prev_) {
      DCHECK(!node->next_);
      node->prev_->next_ = nullptr;
      DCHECK_EQ(node, it->second.tail);
      it->second.tail = node->prev_;
      node->prev_ = nullptr;
    } else {
      DCHECK_EQ(node, it->second.head);
      it->second.head = node->next_;
      DCHECK(node->next_);
      node->next_->prev_ = nullptr;
      node->next_ = nullptr;
    }
  }

  Verify();
}

void AtomicsWaitWakeHandle::Wake() {
  // Adding a separate `NotifyWake()` variant that doesn't acquire the lock
  // itself would likely just add unnecessary complexity..
  // The split lock by itself isn’t an issue, as long as the caller properly
  // synchronizes this with the closing `AtomicsWaitCallback`.
  FutexWaitList* wait_list = GetWaitList();
  {
    replayio::AutoDisallowEvents disallow("AtomicsWaitWakeHandle::Wake");
    NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());
    stopped_ = true;
  }
  isolate_->futex_wait_list_node()->NotifyWake();
}

enum WaitReturnValue : int { kOk = 0, kNotEqualValue = 1, kTimedOut = 2 };

namespace {

Tagged<Object> WaitJsTranslateReturn(Isolate* isolate, Tagged<Object> res) {
  if (IsSmi(res)) {
    int val = Smi::ToInt(res);
    switch (val) {
      case WaitReturnValue::kOk:
        return ReadOnlyRoots(isolate).ok_string();
      case WaitReturnValue::kNotEqualValue:
        return ReadOnlyRoots(isolate).not_equal_string();
      case WaitReturnValue::kTimedOut:
        return ReadOnlyRoots(isolate).timed_out_string();
      default:
        UNREACHABLE();
    }
  }
  return res;
}

}  // namespace

Tagged<Object> FutexEmulation::WaitJs32(Isolate* isolate, WaitMode mode,
                                        Handle<JSArrayBuffer> array_buffer,
                                        size_t addr, int32_t value,
                                        double rel_timeout_ms) {
  Tagged<Object> res =
      Wait<int32_t>(isolate, mode, array_buffer, addr, value, rel_timeout_ms);
  return WaitJsTranslateReturn(isolate, res);
}

Tagged<Object> FutexEmulation::WaitJs64(Isolate* isolate, WaitMode mode,
                                        Handle<JSArrayBuffer> array_buffer,
                                        size_t addr, int64_t value,
                                        double rel_timeout_ms) {
  Tagged<Object> res =
      Wait<int64_t>(isolate, mode, array_buffer, addr, value, rel_timeout_ms);
  return WaitJsTranslateReturn(isolate, res);
}

Tagged<Object> FutexEmulation::WaitWasm32(Isolate* isolate,
                                          Handle<JSArrayBuffer> array_buffer,
                                          size_t addr, int32_t value,
                                          int64_t rel_timeout_ns) {
  return Wait<int32_t>(isolate, WaitMode::kSync, array_buffer, addr, value,
                       rel_timeout_ns >= 0, rel_timeout_ns, CallType::kIsWasm);
}

Tagged<Object> FutexEmulation::WaitWasm64(Isolate* isolate,
                                          Handle<JSArrayBuffer> array_buffer,
                                          size_t addr, int64_t value,
                                          int64_t rel_timeout_ns) {
  return Wait<int64_t>(isolate, WaitMode::kSync, array_buffer, addr, value,
                       rel_timeout_ns >= 0, rel_timeout_ns, CallType::kIsWasm);
}

template <typename T>
Tagged<Object> FutexEmulation::Wait(Isolate* isolate, WaitMode mode,
                                    Handle<JSArrayBuffer> array_buffer,
                                    size_t addr, T value,
                                    double rel_timeout_ms) {
  DCHECK_LT(addr, array_buffer->GetByteLength());

  bool use_timeout = rel_timeout_ms != V8_INFINITY;
  int64_t rel_timeout_ns = -1;

  if (use_timeout) {
    // Convert to nanoseconds.
    double timeout_ns = rel_timeout_ms *
                        base::Time::kNanosecondsPerMicrosecond *
                        base::Time::kMicrosecondsPerMillisecond;
    if (timeout_ns > static_cast<double>(std::numeric_limits<int64_t>::max())) {
      // 2**63 nanoseconds is 292 years. Let's just treat anything greater as
      // infinite.
      use_timeout = false;
    } else {
      rel_timeout_ns = static_cast<int64_t>(timeout_ns);
    }
  }

  recordreplay::AutoAssertMaybeEventsDisallowed assrt("[RUN-2378] FutexEmulation::Wait");

  return Wait(isolate, mode, array_buffer, addr, value, use_timeout,
              rel_timeout_ns);
}

namespace {
double WaitTimeoutInMs(double timeout_ns) {
  return timeout_ns < 0
             ? V8_INFINITY
             : timeout_ns / (base::Time::kNanosecondsPerMicrosecond *
                             base::Time::kMicrosecondsPerMillisecond);
}
}  // namespace

template <typename T>
Tagged<Object> FutexEmulation::Wait(Isolate* isolate, WaitMode mode,
                                    Handle<JSArrayBuffer> array_buffer,
                                    size_t addr, T value, bool use_timeout,
                                    int64_t rel_timeout_ns,
                                    CallType call_type) {
  if (mode == WaitMode::kSync) {
    return WaitSync(isolate, array_buffer, addr, value, use_timeout,
                    rel_timeout_ns, call_type);
  }
  DCHECK_EQ(mode, WaitMode::kAsync);
  return WaitAsync(isolate, array_buffer, addr, value, use_timeout,
                   rel_timeout_ns, call_type);
}

template <typename T>
Tagged<Object> FutexEmulation::WaitSync(Isolate* isolate,
                                        Handle<JSArrayBuffer> array_buffer,
                                        size_t addr, T value, bool use_timeout,
                                        int64_t rel_timeout_ns,
                                        CallType call_type) {
  VMState<ATOMICS_WAIT> state(isolate);
  base::TimeDelta rel_timeout =
      base::TimeDelta::FromNanoseconds(rel_timeout_ns);

  // We have to convert the timeout back to double for the AtomicsWaitCallback.
  double rel_timeout_ms = WaitTimeoutInMs(static_cast<double>(rel_timeout_ns));
  AtomicsWaitWakeHandle stop_handle(isolate);

  isolate->RunAtomicsWaitCallback(AtomicsWaitEvent::kStartWait, array_buffer,
                                  addr, value, rel_timeout_ms, &stop_handle);
  if (isolate->has_exception()) return ReadOnlyRoots(isolate).exception();

  Handle<Object> result;
  AtomicsWaitEvent callback_result = AtomicsWaitEvent::kWokenUp;
  base::Optional<recordreplay::AutoOrderedLock> ordered_lock;
  if (recordreplay::IsRecordingOrReplaying()) {
    ordered_lock.emplace(gRecordReplayOrderedLockId);
  }

  FutexWaitList* wait_list = GetWaitList();
  FutexWaitListNode* node = isolate->futex_wait_list_node();
  void* wait_location = FutexWaitList::ToWaitLocation(*array_buffer, addr);

  base::TimeTicks timeout_time;
  if (use_timeout) {
    base::TimeTicks current_time = base::TimeTicks::Now();
    timeout_time = current_time + rel_timeout;
  }

  // The following is not really a loop; the do-while construct makes it easier
  // to break out early.
  // Keep the code in the loop as minimal as possible, because this is all in
  // the critical section.
  do {
    replayio::AutoDisallowEvents disallow("FutexEmulation::WaitSync");
    NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

    std::atomic<T>* p = reinterpret_cast<std::atomic<T>*>(wait_location);
    T loaded_value = p->load();
#if defined(V8_TARGET_BIG_ENDIAN)
    // If loading a Wasm value, it needs to be reversed on Big Endian platforms.
    if (call_type == CallType::kIsWasm) {
      DCHECK(sizeof(T) == kInt32Size || sizeof(T) == kInt64Size);
      loaded_value = ByteReverse(loaded_value);
    }
#endif
    if (RecordReplayFoundOtherValue(loaded_value != value,
                                    "FutexEmulation::WaitSync")) {
      result = handle(Smi::FromInt(WaitReturnValue::kNotEqualValue), isolate);
      callback_result = AtomicsWaitEvent::kNotEqual;
      break;
    }

    node->wait_location_ = wait_location;
    node->waiting_ = true;
    wait_list->AddNode(node);

    // The ordered lock taken for entering the list is released with events
    // allowed, as OrderedUnlock does nothing otherwise.
    if (ordered_lock) {
      recordreplay::EndDisallowEvents();
      ordered_lock.reset();
      recordreplay::BeginDisallowEventsWithLabel("FutexEmulation::WaitSync");
    }

    while (true) {
      if (V8_UNLIKELY(node->interrupted_)) {
        // Reset the interrupted flag while still holding the mutex.
        node->interrupted_ = false;

        // Unlock the mutex here to prevent deadlock from lock ordering between
        // mutex and mutexes locked by HandleInterrupts.
        lock_guard.Unlock();

        // Because the mutex is unlocked, we have to be careful about not
        // dropping an interrupt. The notification can happen in three different
        // places:
        // 1) Before Wait is called: the notification will be dropped, but
        //    interrupted_ will be set to 1. This will be checked below.
        // 2) After interrupted has been checked here, but before mutex is
        //    acquired: interrupted is checked in a loop, with mutex locked.
        //    Because the wakeup signal also acquires mutex, we know it will not
        //    be able to notify until mutex is released below, when waiting on
        //    the condition variable.
        // 3) After the mutex is released in the call to WaitFor(): this
        //    notification will wake up the condition variable. node->waiting()
        //    will be false, so we'll loop and then check interrupts.
        Tagged<Object> interrupt_object =
            isolate->stack_guard()->HandleInterrupts();

        // When recording or replaying, events are disallowed here, so
        // HandleInterrupts() leaves the interrupts for the next stack check,
        // which a waiter only reaches once woken. A termination left pending
        // would send it back to waiting, so that a worker terminated while
        // blocked here never stopped, so the termination is handled here, with
        // events allowed. Either this thread requested it before waiting (e.g.
        // a worker's process.exit()), which the replay reaches the same way,
        // or another thread did, which invalidated the recording.
        if (!IsException(interrupt_object, isolate) &&
            isolate->stack_guard()->CheckTerminateExecution()) {
          isolate->stack_guard()->ClearTerminateExecution();
          recordreplay::EndDisallowEvents();
          interrupt_object = isolate->TerminateExecution();
          recordreplay::BeginDisallowEventsWithLabel(
              "FutexEmulation::WaitSync");
        }

        lock_guard.Lock();

        if (IsException(interrupt_object, isolate)) {
          result = handle(interrupt_object, isolate);
          callback_result = AtomicsWaitEvent::kTerminatedExecution;
          break;
        }
      }

      if (V8_UNLIKELY(node->interrupted_)) {
        // An interrupt occurred while the mutex was unlocked. Don't wait yet.
        continue;
      }

      if (stop_handle.has_stopped()) {
        node->waiting_ = false;
        callback_result = AtomicsWaitEvent::kAPIStopped;
      }

      if (!node->waiting_) {
        // We were woken either via the stop_handle or via Wake.
        result = handle(Smi::FromInt(WaitReturnValue::kOk), isolate);
        break;
      }

      // No interrupts, now wait.
      if (use_timeout) {
        base::TimeTicks current_time = base::TimeTicks::Now();
        if (current_time >= timeout_time) {
          result = handle(Smi::FromInt(WaitReturnValue::kTimedOut), isolate);
          callback_result = AtomicsWaitEvent::kTimedOut;
          break;
        }

        base::TimeDelta time_until_timeout = timeout_time - current_time;
        DCHECK_GE(time_until_timeout.InMicroseconds(), 0);
        bool wait_for_result =
            node->cond_.WaitFor(wait_list->mutex(), time_until_timeout);
        USE(wait_for_result);
      } else {
        node->cond_.Wait(wait_list->mutex());
      }

      // Spurious wakeup, interrupt or timeout.
    }

    // While recording or replaying, the wait leaves the list under the
    // ordered lock, and whether it timed out is decided again there: the loop
    // decided it with events disallowed, so a replay could time out a wait the
    // recording's notify woke, or the other way around. A notify that takes
    // the lock first wakes the wait even if its timeout has passed.
    if (recordreplay::IsRecordingOrReplaying()) {
      lock_guard.Unlock();
      recordreplay::EndDisallowEvents();
      ordered_lock.emplace(gRecordReplayOrderedLockId);
      recordreplay::BeginDisallowEventsWithLabel("FutexEmulation::WaitSync");
      lock_guard.Lock();
      if (callback_result == AtomicsWaitEvent::kTimedOut && !node->waiting_) {
        result = handle(Smi::FromInt(WaitReturnValue::kOk), isolate);
        callback_result = AtomicsWaitEvent::kWokenUp;
      }
    }

    node->waiting_ = false;
    wait_list->RemoveNode(node);
  } while (false);
  DCHECK(!node->waiting_);
  ordered_lock.reset();

  isolate->RunAtomicsWaitCallback(callback_result, array_buffer, addr, value,
                                  rel_timeout_ms, nullptr);

  if (isolate->has_exception() &&
      callback_result != AtomicsWaitEvent::kTerminatedExecution) {
    return ReadOnlyRoots(isolate).exception();
  }

  return *result;
}

namespace {
template <typename T>
Global<T> GetWeakGlobal(Isolate* isolate, Local<T> object) {
  auto* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8::Global<T> global{v8_isolate, object};
  global.SetWeak();
  return global;
}
}  // namespace

FutexWaitListNode::FutexWaitListNode(std::weak_ptr<BackingStore> backing_store,
                                     void* wait_location,
                                     Handle<JSObject> promise, Isolate* isolate)
    : wait_location_(wait_location),
      waiting_(true),
      async_state_(std::make_unique<AsyncState>(
          isolate,
          V8::GetCurrentPlatform()->GetForegroundTaskRunner(
              reinterpret_cast<v8::Isolate*>(isolate)),
          std::move(backing_store),
          GetWeakGlobal(isolate, Utils::PromiseToLocal(promise)),
          GetWeakGlobal(isolate, Utils::ToLocal(isolate->native_context())))) {}

template <typename T>
Tagged<Object> FutexEmulation::WaitAsync(Isolate* isolate,
                                         Handle<JSArrayBuffer> array_buffer,
                                         size_t addr, T value, bool use_timeout,
                                         int64_t rel_timeout_ns,
                                         CallType call_type) {
  base::TimeDelta rel_timeout =
      base::TimeDelta::FromNanoseconds(rel_timeout_ns);

  Factory* factory = isolate->factory();
  Handle<JSObject> result = factory->NewJSObject(isolate->object_function());
  Handle<JSObject> promise_capability = factory->NewJSPromise();

  enum class ResultKind { kNotEqual, kTimedOut, kAsync };
  ResultKind result_kind;
  RecordReplayTasksToPost record_replay_tasks;
  void* wait_location = FutexWaitList::ToWaitLocation(*array_buffer, addr);
  // Get a weak pointer to the backing store, to be stored in the async state of
  // the node.
  std::weak_ptr<BackingStore> backing_store{array_buffer->GetBackingStore()};
  FutexWaitList* wait_list = GetWaitList();
  {
    recordreplay::AutoOrderedLock ordered_lock(gRecordReplayOrderedLockId);
    replayio::AutoDisallowEvents disallow("FutexEmulation::WaitAsync");
    // 16. Perform EnterCriticalSection(WL).
    NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

    // 17. Let w be ! AtomicLoad(typedArray, i).
    std::atomic<T>* p = static_cast<std::atomic<T>*>(wait_location);
    T loaded_value = p->load();
#if defined(V8_TARGET_BIG_ENDIAN)
    // If loading a Wasm value, it needs to be reversed on Big Endian platforms.
    if (call_type == CallType::kIsWasm) {
      DCHECK(sizeof(T) == kInt32Size || sizeof(T) == kInt64Size);
      loaded_value = ByteReverse(loaded_value);
    }
#endif
    if (RecordReplayFoundOtherValue(loaded_value != value,
                                    "FutexEmulation::WaitAsync")) {
      result_kind = ResultKind::kNotEqual;
    } else if (use_timeout && rel_timeout_ns == 0) {
      result_kind = ResultKind::kTimedOut;
    } else {
      result_kind = ResultKind::kAsync;

      FutexWaitListNode* node = new FutexWaitListNode(
          std::move(backing_store), wait_location, promise_capability, isolate);

      if (use_timeout) {
        node->async_state_->timeout_time = base::TimeTicks::Now() + rel_timeout;
        auto task = std::make_unique<AsyncWaiterTimeoutTask>(
            node->async_state_->isolate_for_async_waiters
                ->cancelable_task_manager(),
            node);
        node->async_state_->timeout_task_id = task->id();
        if (recordreplay::IsRecordingOrReplaying()) {
          record_replay_tasks.Add(node->async_state_->task_runner,
                                  std::move(task), rel_timeout.InSecondsF());
        } else {
          node->async_state_->task_runner->PostNonNestableDelayedTask(
              std::move(task), rel_timeout.InSecondsF());
        }
      }

      wait_list->AddNode(node);
    }

    // Leaving the block collapses the following steps:
    // 18.a. Perform LeaveCriticalSection(WL).
    // 19.b. Perform LeaveCriticalSection(WL).
    // 24. Perform LeaveCriticalSection(WL).
  }

  switch (result_kind) {
    case ResultKind::kNotEqual:
      // 18. If v is not equal to w, then
      //   ...
      //   c. Perform ! CreateDataPropertyOrThrow(resultObject, "async", false).
      //   d. Perform ! CreateDataPropertyOrThrow(resultObject, "value",
      //     "not-equal").
      //   e. Return resultObject.
      CHECK(JSReceiver::CreateDataProperty(
                isolate, result, factory->async_string(),
                factory->false_value(), Just(kDontThrow))
                .FromJust());
      CHECK(JSReceiver::CreateDataProperty(
                isolate, result, factory->value_string(),
                factory->not_equal_string(), Just(kDontThrow))
                .FromJust());
      break;

    case ResultKind::kTimedOut:
      // 19. If t is 0 and mode is async, then
      //   ...
      //   c. Perform ! CreateDataPropertyOrThrow(resultObject, "async", false).
      //   d. Perform ! CreateDataPropertyOrThrow(resultObject, "value",
      //     "timed-out").
      //   e. Return resultObject.
      CHECK(JSReceiver::CreateDataProperty(
                isolate, result, factory->async_string(),
                factory->false_value(), Just(kDontThrow))
                .FromJust());
      CHECK(JSReceiver::CreateDataProperty(
                isolate, result, factory->value_string(),
                factory->timed_out_string(), Just(kDontThrow))
                .FromJust());
      break;

    case ResultKind::kAsync:
      // Add the Promise into the NativeContext's atomics_waitasync_promises
      // set, so that the list keeps it alive.
      Handle<NativeContext> native_context(isolate->native_context());
      Handle<OrderedHashSet> promises(
          native_context->atomics_waitasync_promises(), isolate);
      promises = OrderedHashSet::Add(isolate, promises, promise_capability)
                     .ToHandleChecked();
      native_context->set_atomics_waitasync_promises(*promises);

      // 26. Perform ! CreateDataPropertyOrThrow(resultObject, "async", true).
      // 27. Perform ! CreateDataPropertyOrThrow(resultObject, "value",
      // promiseCapability.[[Promise]]).
      // 28. Return resultObject.
      CHECK(JSReceiver::CreateDataProperty(
                isolate, result, factory->async_string(), factory->true_value(),
                Just(kDontThrow))
                .FromJust());
      CHECK(JSReceiver::CreateDataProperty(isolate, result,
                                           factory->value_string(),
                                           promise_capability, Just(kDontThrow))
                .FromJust());
      break;
  }

  return *result;
}

int FutexEmulation::Wake(Tagged<JSArrayBuffer> array_buffer, size_t addr,
                         uint32_t num_waiters_to_wake) {
  void* wait_location = FutexWaitList::ToWaitLocation(array_buffer, addr);
  return Wake(wait_location, num_waiters_to_wake);
}

int FutexEmulation::Wake(void* wait_location, uint32_t num_waiters_to_wake) {
  int num_waiters_woken = 0;
  FutexWaitList* wait_list = GetWaitList();
  RecordReplayTasksToPost record_replay_tasks;
  recordreplay::AutoOrderedLock ordered_lock(gRecordReplayOrderedLockId);
  replayio::AutoDisallowEvents disallow("FutexEmulation::Wake");
  NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

  auto& location_lists = wait_list->location_lists_;
  auto it = location_lists.find(wait_location);
  if (it == location_lists.end()) return num_waiters_woken;

  FutexWaitListNode* node = it->second.head;
  while (node && num_waiters_to_wake > 0) {
    if (!node->waiting_) {
      node = node->next_;
      continue;
    }
    // Relying on wait_location_ here is not enough, since we need to guard
    // against the case where the BackingStore of the node has been deleted
    // during an async wait and a new BackingStore recreated in the same memory
    // area. Note that sync wait always keeps the backing store alive.
    // It is sufficient to check whether the node's backing store is expired
    // (and consider this a non-match). If it is not expired, it must be
    // identical to the backing store from which wait_location was computed by
    // the caller. In that case, the current context holds the arraybuffer and
    // backing store alive during this call, so it can not expire while we
    // execute this code.
    bool matching_backing_store =
        !node->IsAsync() || !node->async_state_->backing_store.expired();
    if (V8_LIKELY(matching_backing_store)) {
      node->waiting_ = false;

      // Retrieve the next node to iterate before calling NotifyAsyncWaiter,
      // since NotifyAsyncWaiter will take the node out of the linked list.
      FutexWaitListNode* next_node = node->next_;
      if (node->IsAsync()) {
        NotifyAsyncWaiter(node, &record_replay_tasks);
      } else {
        // WaitSync will remove the node from the list.
        node->cond_.NotifyOne();
      }
      node = next_node;
      if (num_waiters_to_wake != kWakeAll) {
        --num_waiters_to_wake;
      }
      num_waiters_woken++;
      continue;
    }

    // ---
    // Code below handles the unlikely case that this node's backing store was
    // deleted during an async wait and a new one was allocated in its place.
    // We delete the node if possible (no timeout, or context is gone).
    // ---
    bool delete_this_node = false;
    DCHECK(node->IsAsync());
    if (node->async_state_->timeout_time.IsNull()) {
      // Backing store has been deleted and the node is still waiting, and
      // there's no timeout. It's never going to be woken up, so we can clean it
      // up now. We don't need to cancel the timeout task, because there is
      // none.

      // This cleanup code is not very efficient, since it only kicks in when
      // a new BackingStore has been created in the same memory area where the
      // deleted BackingStore was.
      DCHECK(node->IsAsync());
      DCHECK_EQ(CancelableTaskManager::kInvalidTaskId,
                node->async_state_->timeout_task_id);
      delete_this_node = true;
    }
    if (node->async_state_->native_context.IsEmpty()) {
      // The NativeContext related to the async waiter has been deleted.
      // Ditto, clean up now.

      // Using the CancelableTaskManager here is OK since the Isolate is
      // guaranteed to be alive - FutexEmulation::IsolateDeinit removes all
      // FutexWaitListNodes owned by an Isolate which is going to die.
      //
      // While recording or replaying, a waiter with a timeout task is left for
      // the task to clean up: whether a notify gets here depends on the
      // collector having freed the waiter's buffer and context and on the
      // allocator putting the notified buffer in its place, so cancelling the
      // task would make whether it runs, and takes the ordered lock, differ
      // between the recording and the replay.
      bool record_replay_leave_to_timeout_task =
          recordreplay::IsRecordingOrReplaying() &&
          node->async_state_->timeout_task_id !=
              CancelableTaskManager::kInvalidTaskId;
      if (!record_replay_leave_to_timeout_task && node->CancelTimeoutTask()) {
        delete_this_node = true;
      }
      // If cancelling the timeout task failed, the timeout task is already
      // running and will clean up the node.
    }

    FutexWaitListNode* next_node = node->next_;
    if (delete_this_node) {
      wait_list->RemoveNode(node);
      delete node;
    }
    node = next_node;
  }

  return num_waiters_woken;
}

void FutexEmulation::CleanupAsyncWaiterPromise(FutexWaitListNode* node) {
  DCHECK(node->IsAsync());
  // This function must run in the main thread of node's Isolate. This function
  // may allocate memory. To avoid deadlocks, we shouldn't be holding the
  // FutexEmulationGlobalState::mutex.

  Isolate* isolate = node->async_state_->isolate_for_async_waiters;
  auto v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);

  if (!node->async_state_->promise.IsEmpty()) {
    Handle<JSPromise> promise = Handle<JSPromise>::cast(
        Utils::OpenHandle(*node->async_state_->promise.Get(v8_isolate)));
    // Promise keeps the NativeContext alive.
    DCHECK(!node->async_state_->native_context.IsEmpty());
    Handle<NativeContext> native_context = Handle<NativeContext>::cast(
        Utils::OpenHandle(*node->async_state_->native_context.Get(v8_isolate)));

    // Remove the Promise from the NativeContext's set.
    Handle<OrderedHashSet> promises(
        native_context->atomics_waitasync_promises(), isolate);
    bool was_deleted = OrderedHashSet::Delete(isolate, *promises, *promise);
    DCHECK(was_deleted);
    USE(was_deleted);
    promises = OrderedHashSet::Shrink(isolate, promises);
    native_context->set_atomics_waitasync_promises(*promises);
  } else {
    // NativeContext keeps the Promise alive; if the Promise is dead then
    // surely NativeContext is too.
    DCHECK(node->async_state_->native_context.IsEmpty());
  }
}

void FutexEmulation::ResolveAsyncWaiterPromise(FutexWaitListNode* node) {
  DCHECK(node->IsAsync());
  // This function must run in the main thread of node's Isolate.

  Isolate* isolate = node->async_state_->isolate_for_async_waiters;
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);

  // Try to cancel the timeout task (if one exists). If the timeout task exists,
  // cancelling it will always succeed. It's not possible for the timeout task
  // to be running, since it's scheduled to run in the same thread as this task.

  // Using the CancelableTaskManager here is OK since the Isolate is guaranteed
  // to be alive - FutexEmulation::IsolateDeinit removes all FutexWaitListNodes
  // owned by an Isolate which is going to die.
  bool success = node->CancelTimeoutTask();
  DCHECK(success);
  USE(success);

  if (!node->async_state_->promise.IsEmpty()) {
    DCHECK(!node->async_state_->native_context.IsEmpty());
    Local<v8::Context> native_context =
        node->async_state_->native_context.Get(v8_isolate);
    v8::Context::Scope contextScope(native_context);
    Handle<JSPromise> promise = Handle<JSPromise>::cast(
        Utils::OpenHandle(*node->async_state_->promise.Get(v8_isolate)));
    Handle<String> result_string;
    // When waiters are notified, their timeout_time is reset. Having a
    // non-zero timeout_time here means the waiter timed out.
    if (node->async_state_->timeout_time != base::TimeTicks()) {
      DCHECK(node->waiting_);
      result_string = isolate->factory()->timed_out_string();
    } else {
      DCHECK(!node->waiting_);
      result_string = isolate->factory()->ok_string();
    }
    MaybeHandle<Object> resolve_result =
        JSPromise::Resolve(promise, result_string);
    DCHECK(!resolve_result.is_null());
    USE(resolve_result);
  }
}

void FutexEmulation::ResolveAsyncWaiterPromises(
    Isolate* isolate, uint64_t record_replay_resolve_id) {
  // This function must run in the main thread of isolate.

  FutexWaitList* wait_list = GetWaitList();
  FutexWaitListNode* node;
  {
    replayio::AutoDisallowEvents disallow("FutexEmulation::ResolveAsyncWaiterPromises");
    NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

    auto& isolate_map = wait_list->isolate_promises_to_resolve_;
    auto it = isolate_map.find(isolate);
    DCHECK_NE(isolate_map.end(), it);

    if (record_replay_resolve_id) {
      node = FutexWaitList::RecordReplayTakeNodesToResolve(
          &it->second, record_replay_resolve_id);
      if (!it->second.head) {
        isolate_map.erase(it);
      }
    } else {
      node = it->second.head;
      isolate_map.erase(it);
    }
  }

  // The list of nodes starting from "node" are no longer on any list, so it's
  // ok to iterate them without holding the mutex. We also need to not hold the
  // mutex while calling CleanupAsyncWaiterPromise, since it may allocate
  // memory.
  HandleScope handle_scope(isolate);
  while (node) {
    DCHECK(node->IsAsync());
    DCHECK_EQ(isolate, node->async_state_->isolate_for_async_waiters);
    DCHECK(!node->waiting_);
    ResolveAsyncWaiterPromise(node);
    CleanupAsyncWaiterPromise(node);
    // We've already tried to cancel the timeout task for the node; since we're
    // now in the same thread the timeout task is supposed to run, we know the
    // timeout task will never happen, and it's safe to delete the node here.
    DCHECK_EQ(CancelableTaskManager::kInvalidTaskId,
              node->async_state_->timeout_task_id);
    node = FutexWaitList::DeleteAsyncWaiterNode(node);
  }
}

void FutexEmulation::HandleAsyncWaiterTimeout(FutexWaitListNode* node) {
  // This function must run in the main thread of node's Isolate.
  DCHECK(node->IsAsync());

  FutexWaitList* wait_list = GetWaitList();

  {
    recordreplay::AutoOrderedLock ordered_lock(gRecordReplayOrderedLockId);
    replayio::AutoDisallowEvents disallow("FutexEmulation::HandleAsyncWaiterTimeout");
    NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

    node->async_state_->timeout_task_id = CancelableTaskManager::kInvalidTaskId;
    if (!node->waiting_) {
      // If the Node is not waiting, it's already scheduled to have its Promise
      // resolved. Ignore the timeout.
      return;
    }
    wait_list->RemoveNode(node);
  }

  // "node" has been taken out of the lists, so it's ok to access it without
  // holding the mutex. We also need to not hold the mutex while calling
  // CleanupAsyncWaiterPromise, since it may allocate memory.
  HandleScope handle_scope(node->async_state_->isolate_for_async_waiters);
  ResolveAsyncWaiterPromise(node);
  CleanupAsyncWaiterPromise(node);
  delete node;
}

void FutexEmulation::IsolateDeinit(Isolate* isolate) {
  recordreplay::AutoOrderedLock ordered_lock(gRecordReplayOrderedLockId);
  replayio::AutoDisallowEvents disallow("FutexEmulation::IsolateDeinit");
  FutexWaitList* wait_list = GetWaitList();
  NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

  // Iterate all locations to find nodes belonging to "isolate" and delete them.
  // The Isolate is going away; don't bother cleaning up the Promises in the
  // NativeContext. Also we don't need to cancel the timeout tasks, since they
  // will be cancelled by Isolate::Deinit.
  {
    auto& location_lists = wait_list->location_lists_;
    auto it = location_lists.begin();
    while (it != location_lists.end()) {
      FutexWaitListNode*& head = it->second.head;
      FutexWaitListNode*& tail = it->second.tail;
      FutexWaitList::DeleteNodesForIsolate(isolate, &head, &tail);
      // head and tail are either both nullptr or both non-nullptr.
      DCHECK_EQ(head == nullptr, tail == nullptr);
      if (head == nullptr) {
        it = location_lists.erase(it);
      } else {
        ++it;
      }
    }
  }

  {
    auto& isolate_map = wait_list->isolate_promises_to_resolve_;
    auto it = isolate_map.find(isolate);
    if (it != isolate_map.end()) {
      for (FutexWaitListNode* node = it->second.head; node;) {
        DCHECK(node->IsAsync());
        DCHECK_EQ(isolate, node->async_state_->isolate_for_async_waiters);
        node->async_state_->timeout_task_id =
            CancelableTaskManager::kInvalidTaskId;
        node = FutexWaitList::DeleteAsyncWaiterNode(node);
      }
      isolate_map.erase(it);
    }
  }

  wait_list->Verify();
}

int FutexEmulation::NumWaitersForTesting(Tagged<JSArrayBuffer> array_buffer,
                                         size_t addr) {
  void* wait_location = FutexWaitList::ToWaitLocation(*array_buffer, addr);
  replayio::AutoDisallowEvents disallow("FutexEmulation::NumWaitersForTesting");
  FutexWaitList* wait_list = GetWaitList();
  NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

  int num_waiters = 0;
  auto& location_lists = wait_list->location_lists_;
  auto it = location_lists.find(wait_location);
  if (it == location_lists.end()) return num_waiters;

  for (FutexWaitListNode* node = it->second.head; node; node = node->next_) {
    if (!node->waiting_) continue;
    if (node->IsAsync()) {
      if (node->async_state_->backing_store.expired()) continue;
      DCHECK_EQ(array_buffer->GetBackingStore(),
                node->async_state_->backing_store.lock());
    }
    num_waiters++;
  }

  return num_waiters;
}

int FutexEmulation::NumAsyncWaitersForTesting(Isolate* isolate) {
  replayio::AutoDisallowEvents disallow("FutexEmulation::NumAsyncWaitersForTesting");
  FutexWaitList* wait_list = GetWaitList();
  NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

  int num_waiters = 0;
  for (const auto& it : wait_list->location_lists_) {
    for (FutexWaitListNode* node = it.second.head; node; node = node->next_) {
      if (!node->IsAsync()) continue;
      if (!node->waiting_) continue;
      if (node->async_state_->isolate_for_async_waiters != isolate) continue;
      num_waiters++;
    }
  }

  return num_waiters;
}

int FutexEmulation::NumUnresolvedAsyncPromisesForTesting(
    Tagged<JSArrayBuffer> array_buffer, size_t addr) {
  void* wait_location = FutexWaitList::ToWaitLocation(array_buffer, addr);
  replayio::AutoDisallowEvents disallow("FutexEmulation::NumUnresolvedAsyncPromisesForTesting");
  FutexWaitList* wait_list = GetWaitList();
  NoGarbageCollectionMutexGuard lock_guard(wait_list->mutex());

  int num_waiters = 0;
  auto& isolate_map = wait_list->isolate_promises_to_resolve_;
  for (const auto& it : isolate_map) {
    for (FutexWaitListNode* node = it.second.head; node; node = node->next_) {
      DCHECK(node->IsAsync());
      if (node->waiting_) continue;
      if (wait_location != node->wait_location_) continue;
      if (node->async_state_->backing_store.expired()) continue;
      DCHECK_EQ(array_buffer->GetBackingStore(),
                node->async_state_->backing_store.lock());
      num_waiters++;
    }
  }

  return num_waiters;
}

void FutexWaitList::Verify() const {
#ifdef DEBUG
  auto VerifyNode = [](FutexWaitListNode* node, FutexWaitListNode* head,
                       FutexWaitListNode* tail) {
    if (node->next_ != nullptr) {
      DCHECK_NE(node, tail);
      DCHECK_EQ(node, node->next_->prev_);
    } else {
      DCHECK_EQ(node, tail);
    }
    if (node->prev_ != nullptr) {
      DCHECK_NE(node, head);
      DCHECK_EQ(node, node->prev_->next_);
    } else {
      DCHECK_EQ(node, head);
    }

    DCHECK(NodeIsOnList(node, head));
  };

  for (const auto& [addr, head_and_tail] : location_lists_) {
    auto [head, tail] = head_and_tail;
    for (FutexWaitListNode* node = head; node; node = node->next_) {
      VerifyNode(node, head, tail);
    }
  }

  for (const auto& [isolate, head_and_tail] : isolate_promises_to_resolve_) {
    auto [head, tail] = head_and_tail;
    for (FutexWaitListNode* node = head; node; node = node->next_) {
      DCHECK(node->IsAsync());
      VerifyNode(node, head, tail);
      DCHECK_EQ(isolate, node->async_state_->isolate_for_async_waiters);
    }
  }
#endif  // DEBUG
}

bool FutexWaitList::NodeIsOnList(FutexWaitListNode* node,
                                 FutexWaitListNode* head) {
  for (FutexWaitListNode* n = head; n; n = n->next_) {
    if (n == node) return true;
  }
  return false;
}

}  // namespace v8::internal
