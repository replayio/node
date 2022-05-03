// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/debug/debug.h"

#include <memory>
#include <unordered_set>

#include "src/api/api-inl.h"
#include "src/api/api-natives.h"
#include "src/base/platform/mutex.h"
#include "src/builtins/builtins.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/compilation-cache.h"
#include "src/codegen/compiler.h"
#include "src/common/globals.h"
#include "src/common/message-template.h"
#include "src/debug/debug-evaluate.h"
#include "src/debug/liveedit.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/arguments.h"
#include "src/execution/execution.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/execution/v8threads.h"
#include "src/handles/global-handles.h"
#include "src/heap/heap-inl.h"  // For NextDebuggingId.
#include "src/init/bootstrapper.h"
#include "src/inspector/v8-debugger-agent-impl.h"
#include "src/inspector/v8-inspector-impl.h"
#include "src/inspector/v8-inspector-session-impl.h"
#include "src/inspector/v8-runtime-agent-impl.h"
#include "src/interpreter/bytecode-array-iterator.h"
#include "src/interpreter/interpreter.h"
#include "src/json/json-parser.h"
#include "src/json/json-stringifier.h"
#include "src/logging/counters.h"
#include "src/logging/runtime-call-stats-scope.h"
#include "src/objects/api-callbacks-inl.h"
#include "src/objects/debug-objects-inl.h"
#include "src/objects/js-generator-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/slots.h"
#include "src/snapshot/embedded/embedded-data.h"
#include "src/snapshot/snapshot.h"
#include "src/tasks/task-utils.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-debug.h"
#include "src/wasm/wasm-objects-inl.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {

class Debug::TemporaryObjectsTracker : public HeapObjectAllocationTracker {
 public:
  TemporaryObjectsTracker() = default;
  ~TemporaryObjectsTracker() override = default;
  TemporaryObjectsTracker(const TemporaryObjectsTracker&) = delete;
  TemporaryObjectsTracker& operator=(const TemporaryObjectsTracker&) = delete;

  void AllocationEvent(Address addr, int) override {
    if (disabled) return;
    objects_.insert(addr);
  }

  void MoveEvent(Address from, Address to, int) override {
    if (from == to) return;
    base::MutexGuard guard(&mutex_);
    auto it = objects_.find(from);
    if (it == objects_.end()) {
      // If temporary object was collected we can get MoveEvent which moves
      // existing non temporary object to the address where we had temporary
      // object. So we should mark new address as non temporary.
      objects_.erase(to);
      return;
    }
    objects_.erase(it);
    objects_.insert(to);
  }

  bool HasObject(Handle<HeapObject> obj) const {
    if (obj->IsJSObject() &&
        Handle<JSObject>::cast(obj)->GetEmbedderFieldCount()) {
      // Embedder may store any pointers using embedder fields and implements
      // non trivial logic, e.g. create wrappers lazily and store pointer to
      // native object inside embedder field. We should consider all objects
      // with embedder fields as non temporary.
      return false;
    }
    return objects_.find(obj->address()) != objects_.end();
  }

  bool disabled = false;

 private:
  std::unordered_set<Address> objects_;
  base::Mutex mutex_;
};

Debug::Debug(Isolate* isolate)
    : is_active_(false),
      hook_on_function_call_(false),
      is_suppressed_(false),
      break_disabled_(false),
      break_points_active_(true),
      break_on_exception_(false),
      break_on_uncaught_exception_(false),
      side_effect_check_failed_(false),
      debug_info_list_(nullptr),
      feature_tracker_(isolate),
      isolate_(isolate) {
  ThreadInit();
}

Debug::~Debug() { DCHECK_NULL(debug_delegate_); }

BreakLocation BreakLocation::FromFrame(Handle<DebugInfo> debug_info,
                                       JavaScriptFrame* frame) {
  if (debug_info->CanBreakAtEntry()) {
    return BreakLocation(Debug::kBreakAtEntryPosition, DEBUG_BREAK_AT_ENTRY);
  }
  auto summary = FrameSummary::GetTop(frame).AsJavaScript();
  int offset = summary.code_offset();
  Handle<AbstractCode> abstract_code = summary.abstract_code();
  BreakIterator it(debug_info);
  it.SkipTo(BreakIndexFromCodeOffset(debug_info, abstract_code, offset));
  return it.GetBreakLocation();
}

void BreakLocation::AllAtCurrentStatement(
    Handle<DebugInfo> debug_info, JavaScriptFrame* frame,
    std::vector<BreakLocation>* result_out) {
  DCHECK(!debug_info->CanBreakAtEntry());
  auto summary = FrameSummary::GetTop(frame).AsJavaScript();
  int offset = summary.code_offset();
  Handle<AbstractCode> abstract_code = summary.abstract_code();
  if (abstract_code->IsCode()) offset = offset - 1;
  int statement_position;
  {
    BreakIterator it(debug_info);
    it.SkipTo(BreakIndexFromCodeOffset(debug_info, abstract_code, offset));
    statement_position = it.statement_position();
  }
  for (BreakIterator it(debug_info); !it.Done(); it.Next()) {
    if (it.statement_position() == statement_position) {
      result_out->push_back(it.GetBreakLocation());
    }
  }
}

JSGeneratorObject BreakLocation::GetGeneratorObjectForSuspendedFrame(
    JavaScriptFrame* frame) const {
  DCHECK(IsSuspend());
  DCHECK_GE(generator_obj_reg_index_, 0);

  Object generator_obj = UnoptimizedFrame::cast(frame)->ReadInterpreterRegister(
      generator_obj_reg_index_);

  return JSGeneratorObject::cast(generator_obj);
}

int BreakLocation::BreakIndexFromCodeOffset(Handle<DebugInfo> debug_info,
                                            Handle<AbstractCode> abstract_code,
                                            int offset) {
  // Run through all break points to locate the one closest to the address.
  int closest_break = 0;
  int distance = kMaxInt;
  DCHECK(0 <= offset && offset < abstract_code->Size());
  for (BreakIterator it(debug_info); !it.Done(); it.Next()) {
    // Check if this break point is closer that what was previously found.
    if (it.code_offset() <= offset && offset - it.code_offset() < distance) {
      closest_break = it.break_index();
      distance = offset - it.code_offset();
      // Check whether we can't get any closer.
      if (distance == 0) break;
    }
  }
  return closest_break;
}

bool BreakLocation::HasBreakPoint(Isolate* isolate,
                                  Handle<DebugInfo> debug_info) const {
  // First check whether there is a break point with the same source position.
  if (!debug_info->HasBreakPoint(isolate, position_)) return false;
  if (debug_info->CanBreakAtEntry()) {
    DCHECK_EQ(Debug::kBreakAtEntryPosition, position_);
    return debug_info->BreakAtEntry();
  } else {
    // Then check whether a break point at that source position would have
    // the same code offset. Otherwise it's just a break location that we can
    // step to, but not actually a location where we can put a break point.
    DCHECK(abstract_code_->IsBytecodeArray());
    BreakIterator it(debug_info);
    it.SkipToPosition(position_);
    return it.code_offset() == code_offset_;
  }
}

debug::BreakLocationType BreakLocation::type() const {
  switch (type_) {
    case DEBUGGER_STATEMENT:
      return debug::kDebuggerStatementBreakLocation;
    case DEBUG_BREAK_SLOT_AT_CALL:
      return debug::kCallBreakLocation;
    case DEBUG_BREAK_SLOT_AT_RETURN:
      return debug::kReturnBreakLocation;

    // Externally, suspend breaks should look like normal breaks.
    case DEBUG_BREAK_SLOT_AT_SUSPEND:
    default:
      return debug::kCommonBreakLocation;
  }
}

BreakIterator::BreakIterator(Handle<DebugInfo> debug_info)
    : debug_info_(debug_info),
      break_index_(-1),
      source_position_iterator_(
          debug_info->DebugBytecodeArray().SourcePositionTable()) {
  position_ = debug_info->shared().StartPosition();
  statement_position_ = position_;
  // There is at least one break location.
  DCHECK(!Done());
  Next();
}

int BreakIterator::BreakIndexFromPosition(int source_position) {
  for (; !Done(); Next()) {
    if (source_position <= position()) {
      int first_break = break_index();
      for (; !Done(); Next()) {
        if (source_position == position()) return break_index();
      }
      return first_break;
    }
  }
  return break_index();
}

void BreakIterator::Next() {
  DisallowGarbageCollection no_gc;
  DCHECK(!Done());
  bool first = break_index_ == -1;
  while (!Done()) {
    if (!first) source_position_iterator_.Advance();
    first = false;
    if (Done()) return;
    position_ = source_position_iterator_.source_position().ScriptOffset();
    if (source_position_iterator_.is_statement()) {
      statement_position_ = position_;
    }
    DCHECK_LE(0, position_);
    DCHECK_LE(0, statement_position_);

    DebugBreakType type = GetDebugBreakType();
    if (type != NOT_DEBUG_BREAK) break;
  }
  break_index_++;
}

DebugBreakType BreakIterator::GetDebugBreakType() {
  BytecodeArray bytecode_array = debug_info_->OriginalBytecodeArray();
  interpreter::Bytecode bytecode =
      interpreter::Bytecodes::FromByte(bytecode_array.get(code_offset()));

  // Make sure we read the actual bytecode, not a prefix scaling bytecode.
  if (interpreter::Bytecodes::IsPrefixScalingBytecode(bytecode)) {
    bytecode =
        interpreter::Bytecodes::FromByte(bytecode_array.get(code_offset() + 1));
  }

  if (bytecode == interpreter::Bytecode::kDebugger) {
    return DEBUGGER_STATEMENT;
  } else if (bytecode == interpreter::Bytecode::kReturn) {
    return DEBUG_BREAK_SLOT_AT_RETURN;
  } else if (bytecode == interpreter::Bytecode::kSuspendGenerator) {
    return DEBUG_BREAK_SLOT_AT_SUSPEND;
  } else if (interpreter::Bytecodes::IsCallOrConstruct(bytecode)) {
    return DEBUG_BREAK_SLOT_AT_CALL;
  } else if (source_position_iterator_.is_statement()) {
    return DEBUG_BREAK_SLOT;
  } else {
    return NOT_DEBUG_BREAK;
  }
}

void BreakIterator::SkipToPosition(int position) {
  BreakIterator it(debug_info_);
  SkipTo(it.BreakIndexFromPosition(position));
}

void BreakIterator::SetDebugBreak() {
  DebugBreakType debug_break_type = GetDebugBreakType();
  if (debug_break_type == DEBUGGER_STATEMENT) return;
  HandleScope scope(isolate());
  DCHECK(debug_break_type >= DEBUG_BREAK_SLOT);
  Handle<BytecodeArray> bytecode_array(debug_info_->DebugBytecodeArray(),
                                       isolate());
  interpreter::BytecodeArrayIterator(bytecode_array, code_offset())
      .ApplyDebugBreak();
}

void BreakIterator::ClearDebugBreak() {
  DebugBreakType debug_break_type = GetDebugBreakType();
  if (debug_break_type == DEBUGGER_STATEMENT) return;
  DCHECK(debug_break_type >= DEBUG_BREAK_SLOT);
  BytecodeArray bytecode_array = debug_info_->DebugBytecodeArray();
  BytecodeArray original = debug_info_->OriginalBytecodeArray();
  bytecode_array.set(code_offset(), original.get(code_offset()));
}

BreakLocation BreakIterator::GetBreakLocation() {
  Handle<AbstractCode> code(
      AbstractCode::cast(debug_info_->DebugBytecodeArray()), isolate());
  DebugBreakType type = GetDebugBreakType();
  int generator_object_reg_index = -1;
  int generator_suspend_id = -1;
  if (type == DEBUG_BREAK_SLOT_AT_SUSPEND) {
    // For suspend break, we'll need the generator object to be able to step
    // over the suspend as if it didn't return. We get the interpreter register
    // index that holds the generator object by reading it directly off the
    // bytecode array, and we'll read the actual generator object off the
    // interpreter stack frame in GetGeneratorObjectForSuspendedFrame.
    BytecodeArray bytecode_array = debug_info_->OriginalBytecodeArray();
    interpreter::BytecodeArrayIterator iterator(
        handle(bytecode_array, isolate()), code_offset());

    DCHECK_EQ(iterator.current_bytecode(),
              interpreter::Bytecode::kSuspendGenerator);
    interpreter::Register generator_obj_reg = iterator.GetRegisterOperand(0);
    generator_object_reg_index = generator_obj_reg.index();

    // Also memorize the suspend ID, to be able to decide whether
    // we are paused on the implicit initial yield later.
    generator_suspend_id = iterator.GetUnsignedImmediateOperand(3);
  }
  return BreakLocation(code, type, code_offset(), position_,
                       generator_object_reg_index, generator_suspend_id);
}

Isolate* BreakIterator::isolate() { return debug_info_->GetIsolate(); }

void DebugFeatureTracker::Track(DebugFeatureTracker::Feature feature) {
  uint32_t mask = 1 << feature;
  // Only count one sample per feature and isolate.
  if (bitfield_ & mask) return;
  isolate_->counters()->debug_feature_usage()->AddSample(feature);
  bitfield_ |= mask;
}

// Threading support.
void Debug::ThreadInit() {
  thread_local_.break_frame_id_ = StackFrameId::NO_ID;
  thread_local_.last_step_action_ = StepNone;
  thread_local_.last_statement_position_ = kNoSourcePosition;
  thread_local_.last_frame_count_ = -1;
  thread_local_.fast_forward_to_return_ = false;
  thread_local_.ignore_step_into_function_ = Smi::zero();
  thread_local_.target_frame_count_ = -1;
  thread_local_.return_value_ = Smi::zero();
  thread_local_.last_breakpoint_id_ = 0;
  clear_suspended_generator();
  base::Relaxed_Store(&thread_local_.current_debug_scope_,
                      static_cast<base::AtomicWord>(0));
  thread_local_.break_on_next_function_call_ = false;
  UpdateHookOnFunctionCall();
}

char* Debug::ArchiveDebug(char* storage) {
  MemCopy(storage, reinterpret_cast<char*>(&thread_local_),
          ArchiveSpacePerThread());
  return storage + ArchiveSpacePerThread();
}

char* Debug::RestoreDebug(char* storage) {
  MemCopy(reinterpret_cast<char*>(&thread_local_), storage,
          ArchiveSpacePerThread());

  // Enter the debugger.
  DebugScope debug_scope(this);

  // Clear any one-shot breakpoints that may have been set by the other
  // thread, and reapply breakpoints for this thread.
  ClearOneShot();

  if (thread_local_.last_step_action_ != StepNone) {
    int current_frame_count = CurrentFrameCount();
    int target_frame_count = thread_local_.target_frame_count_;
    DCHECK(current_frame_count >= target_frame_count);
    StackTraceFrameIterator frames_it(isolate_);
    while (current_frame_count > target_frame_count) {
      current_frame_count -= frames_it.FrameFunctionCount();
      frames_it.Advance();
    }
    DCHECK(current_frame_count == target_frame_count);
    // Set frame to what it was at Step break
    thread_local_.break_frame_id_ = frames_it.frame()->id();

    // Reset the previous step action for this thread.
    PrepareStep(thread_local_.last_step_action_);
  }

  return storage + ArchiveSpacePerThread();
}

int Debug::ArchiveSpacePerThread() { return sizeof(ThreadLocal); }

void Debug::Iterate(RootVisitor* v) { Iterate(v, &thread_local_); }

char* Debug::Iterate(RootVisitor* v, char* thread_storage) {
  ThreadLocal* thread_local_data =
      reinterpret_cast<ThreadLocal*>(thread_storage);
  Iterate(v, thread_local_data);
  return thread_storage + ArchiveSpacePerThread();
}

void Debug::Iterate(RootVisitor* v, ThreadLocal* thread_local_data) {
  v->VisitRootPointer(Root::kDebug, nullptr,
                      FullObjectSlot(&thread_local_data->return_value_));
  v->VisitRootPointer(Root::kDebug, nullptr,
                      FullObjectSlot(&thread_local_data->suspended_generator_));
  v->VisitRootPointer(
      Root::kDebug, nullptr,
      FullObjectSlot(&thread_local_data->ignore_step_into_function_));
}

DebugInfoListNode::DebugInfoListNode(Isolate* isolate, DebugInfo debug_info)
    : next_(nullptr) {
  // Globalize the request debug info object and make it weak.
  GlobalHandles* global_handles = isolate->global_handles();
  debug_info_ = global_handles->Create(debug_info).location();
}

DebugInfoListNode::~DebugInfoListNode() {
  if (debug_info_ == nullptr) return;
  GlobalHandles::Destroy(debug_info_);
  debug_info_ = nullptr;
}

void Debug::Unload() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  ClearAllBreakPoints();
  ClearStepping();
  RemoveAllCoverageInfos();
  ClearAllDebuggerHints();
  debug_delegate_ = nullptr;
}

void Debug::Break(JavaScriptFrame* frame, Handle<JSFunction> break_target) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Just continue if breaks are disabled or debugger cannot be loaded.
  if (break_disabled()) return;

  // Enter the debugger.
  DebugScope debug_scope(this);
  DisableBreak no_recursive_break(this);

  // Return if we fail to retrieve debug info.
  Handle<SharedFunctionInfo> shared(break_target->shared(), isolate_);
  if (!EnsureBreakInfo(shared)) return;
  PrepareFunctionForDebugExecution(shared);

  Handle<DebugInfo> debug_info(shared->GetDebugInfo(), isolate_);

  // Find the break location where execution has stopped.
  BreakLocation location = BreakLocation::FromFrame(debug_info, frame);

  // Find actual break points, if any, and trigger debug break event.
  MaybeHandle<FixedArray> break_points_hit =
      CheckBreakPoints(debug_info, &location);
  if (!break_points_hit.is_null() || break_on_next_function_call()) {
    StepAction lastStepAction = last_step_action();
    // Clear all current stepping setup.
    ClearStepping();
    // Notify the debug event listeners.
    OnDebugBreak(!break_points_hit.is_null()
                     ? break_points_hit.ToHandleChecked()
                     : isolate_->factory()->empty_fixed_array(),
                 lastStepAction);
    return;
  }

  // Debug break at function entry, do not worry about stepping.
  if (location.IsDebugBreakAtEntry()) {
    DCHECK(debug_info->BreakAtEntry());
    return;
  }

  DCHECK_NOT_NULL(frame);

  // No break point. Check for stepping.
  StepAction step_action = last_step_action();
  int current_frame_count = CurrentFrameCount();
  int target_frame_count = thread_local_.target_frame_count_;
  int last_frame_count = thread_local_.last_frame_count_;

  // StepOut at not return position was requested and return break locations
  // were flooded with one shots.
  if (thread_local_.fast_forward_to_return_) {
    DCHECK(location.IsReturnOrSuspend());
    // We have to ignore recursive calls to function.
    if (current_frame_count > target_frame_count) return;
    ClearStepping();
    PrepareStep(StepOut);
    return;
  }

  bool step_break = false;
  switch (step_action) {
    case StepNone:
      return;
    case StepOut:
      // StepOut should not break in a deeper frame than target frame.
      if (current_frame_count > target_frame_count) return;
      step_break = true;
      break;
    case StepOver:
      // StepOver should not break in a deeper frame than target frame.
      if (current_frame_count > target_frame_count) return;
      V8_FALLTHROUGH;
    case StepInto: {
      // Special case StepInto and StepOver for generators that are about to
      // suspend, in which case we go into "generator stepping" mode. The
      // exception here is the initial implicit yield in generators (which
      // always has a suspend ID of 0), where we return to the caller first,
      // instead of triggering "generator stepping" mode straight away.
      if (location.IsSuspend() && (!IsGeneratorFunction(shared->kind()) ||
                                   location.generator_suspend_id() > 0)) {
        DCHECK(!has_suspended_generator());
        thread_local_.suspended_generator_ =
            location.GetGeneratorObjectForSuspendedFrame(frame);
        ClearStepping();
        return;
      }

      FrameSummary summary = FrameSummary::GetTop(frame);
      step_break = step_break || location.IsReturn() ||
                   current_frame_count != last_frame_count ||
                   thread_local_.last_statement_position_ !=
                       summary.SourceStatementPosition();
      break;
    }
  }

  StepAction lastStepAction = last_step_action();
  // Clear all current stepping setup.
  ClearStepping();

  if (step_break) {
    // Notify the debug event listeners.
    OnDebugBreak(isolate_->factory()->empty_fixed_array(), lastStepAction);
  } else {
    // Re-prepare to continue.
    PrepareStep(step_action);
  }
}

// Find break point objects for this location, if any, and evaluate them.
// Return an array of break point objects that evaluated true, or an empty
// handle if none evaluated true.
MaybeHandle<FixedArray> Debug::CheckBreakPoints(Handle<DebugInfo> debug_info,
                                                BreakLocation* location,
                                                bool* has_break_points) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  bool has_break_points_to_check =
      break_points_active_ && location->HasBreakPoint(isolate_, debug_info);
  if (has_break_points) *has_break_points = has_break_points_to_check;
  if (!has_break_points_to_check) return {};

  return Debug::GetHitBreakPoints(debug_info, location->position());
}

bool Debug::IsMutedAtCurrentLocation(JavaScriptFrame* frame) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);
  // A break location is considered muted if break locations on the current
  // statement have at least one break point, and all of these break points
  // evaluate to false. Aside from not triggering a debug break event at the
  // break location, we also do not trigger one for debugger statements, nor
  // an exception event on exception at this location.
  FrameSummary summary = FrameSummary::GetTop(frame);
  Handle<JSFunction> function = summary.AsJavaScript().function();
  if (!function->shared().HasBreakInfo()) return false;
  Handle<DebugInfo> debug_info(function->shared().GetDebugInfo(), isolate_);
  // Enter the debugger.
  DebugScope debug_scope(this);
  std::vector<BreakLocation> break_locations;
  BreakLocation::AllAtCurrentStatement(debug_info, frame, &break_locations);
  bool has_break_points_at_all = false;
  for (size_t i = 0; i < break_locations.size(); i++) {
    bool has_break_points;
    MaybeHandle<FixedArray> check_result =
        CheckBreakPoints(debug_info, &break_locations[i], &has_break_points);
    has_break_points_at_all |= has_break_points;
    if (has_break_points && !check_result.is_null()) return false;
  }
  return has_break_points_at_all;
}

// Check whether a single break point object is triggered.
bool Debug::CheckBreakPoint(Handle<BreakPoint> break_point,
                            bool is_break_at_entry) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);

  if (!break_point->condition().length()) return true;
  Handle<String> condition(break_point->condition(), isolate_);
  MaybeHandle<Object> maybe_result;
  Handle<Object> result;

  if (is_break_at_entry) {
    maybe_result = DebugEvaluate::WithTopmostArguments(isolate_, condition);
  } else {
    // Since we call CheckBreakpoint only for deoptimized frame on top of stack,
    // we can use 0 as index of inlined frame.
    const int inlined_jsframe_index = 0;
    const bool throw_on_side_effect = false;
    maybe_result =
        DebugEvaluate::Local(isolate_, break_frame_id(), inlined_jsframe_index,
                             condition, throw_on_side_effect);
  }

  if (!maybe_result.ToHandle(&result)) {
    if (isolate_->has_pending_exception()) {
      isolate_->clear_pending_exception();
    }
    return false;
  }
  return result->BooleanValue(isolate_);
}

bool Debug::SetBreakpoint(Handle<SharedFunctionInfo> shared,
                          Handle<BreakPoint> break_point,
                          int* source_position) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);

  // Make sure the function is compiled and has set up the debug info.
  if (!EnsureBreakInfo(shared)) return false;
  PrepareFunctionForDebugExecution(shared);

  Handle<DebugInfo> debug_info(shared->GetDebugInfo(), isolate_);
  // Source positions starts with zero.
  DCHECK_LE(0, *source_position);

  // Find the break point and change it.
  *source_position = FindBreakablePosition(debug_info, *source_position);
  DebugInfo::SetBreakPoint(isolate_, debug_info, *source_position, break_point);
  // At least one active break point now.
  DCHECK_LT(0, debug_info->GetBreakPointCount(isolate_));

  ClearBreakPoints(debug_info);
  ApplyBreakPoints(debug_info);

  feature_tracker()->Track(DebugFeatureTracker::kBreakPoint);
  return true;
}

bool Debug::SetBreakPointForScript(Handle<Script> script,
                                   Handle<String> condition,
                                   int* source_position, int* id) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  *id = ++thread_local_.last_breakpoint_id_;
  Handle<BreakPoint> break_point =
      isolate_->factory()->NewBreakPoint(*id, condition);
#if V8_ENABLE_WEBASSEMBLY
  if (script->type() == Script::TYPE_WASM) {
    RecordWasmScriptWithBreakpoints(script);
    return WasmScript::SetBreakPoint(script, source_position, break_point);
  }
#endif  //  V8_ENABLE_WEBASSEMBLY

  HandleScope scope(isolate_);

  // Obtain shared function info for the innermost function containing this
  // position.
  Handle<Object> result =
      FindInnermostContainingFunctionInfo(script, *source_position);
  if (result->IsUndefined(isolate_)) return false;

  auto shared = Handle<SharedFunctionInfo>::cast(result);
  if (!EnsureBreakInfo(shared)) return false;
  PrepareFunctionForDebugExecution(shared);

  // Find the nested shared function info that is closest to the position within
  // the containing function.
  shared = FindClosestSharedFunctionInfoFromPosition(*source_position, script,
                                                     shared);

  // Set the breakpoint in the function.
  return SetBreakpoint(shared, break_point, source_position);
}

int Debug::FindBreakablePosition(Handle<DebugInfo> debug_info,
                                 int source_position) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (debug_info->CanBreakAtEntry()) {
    return kBreakAtEntryPosition;
  } else {
    DCHECK(debug_info->HasInstrumentedBytecodeArray());
    BreakIterator it(debug_info);
    it.SkipToPosition(source_position);
    return it.position();
  }
}

void Debug::ApplyBreakPoints(Handle<DebugInfo> debug_info) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DisallowGarbageCollection no_gc;
  if (debug_info->CanBreakAtEntry()) {
    debug_info->SetBreakAtEntry();
  } else {
    if (!debug_info->HasInstrumentedBytecodeArray()) return;
    FixedArray break_points = debug_info->break_points();
    for (int i = 0; i < break_points.length(); i++) {
      if (break_points.get(i).IsUndefined(isolate_)) continue;
      BreakPointInfo info = BreakPointInfo::cast(break_points.get(i));
      if (info.GetBreakPointCount(isolate_) == 0) continue;
      DCHECK(debug_info->HasInstrumentedBytecodeArray());
      BreakIterator it(debug_info);
      it.SkipToPosition(info.source_position());
      it.SetDebugBreak();
    }
  }
  debug_info->SetDebugExecutionMode(DebugInfo::kBreakpoints);
}

void Debug::ClearBreakPoints(Handle<DebugInfo> debug_info) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (debug_info->CanBreakAtEntry()) {
    debug_info->ClearBreakAtEntry();
  } else {
    // If we attempt to clear breakpoints but none exist, simply return. This
    // can happen e.g. CoverageInfos exist but no breakpoints are set.
    if (!debug_info->HasInstrumentedBytecodeArray() ||
        !debug_info->HasBreakInfo()) {
      return;
    }

    DisallowGarbageCollection no_gc;
    for (BreakIterator it(debug_info); !it.Done(); it.Next()) {
      it.ClearDebugBreak();
    }
  }
}

void Debug::ClearBreakPoint(Handle<BreakPoint> break_point) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);

  for (DebugInfoListNode* node = debug_info_list_; node != nullptr;
       node = node->next()) {
    if (!node->debug_info()->HasBreakInfo()) continue;
    Handle<Object> result = DebugInfo::FindBreakPointInfo(
        isolate_, node->debug_info(), break_point);
    if (result->IsUndefined(isolate_)) continue;
    Handle<DebugInfo> debug_info = node->debug_info();
    if (DebugInfo::ClearBreakPoint(isolate_, debug_info, break_point)) {
      ClearBreakPoints(debug_info);
      if (debug_info->GetBreakPointCount(isolate_) == 0) {
        RemoveBreakInfoAndMaybeFree(debug_info);
      } else {
        ApplyBreakPoints(debug_info);
      }
      return;
    }
  }
}

int Debug::GetFunctionDebuggingId(Handle<JSFunction> function) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  Handle<SharedFunctionInfo> shared = handle(function->shared(), isolate_);
  Handle<DebugInfo> debug_info = GetOrCreateDebugInfo(shared);
  int id = debug_info->debugging_id();
  if (id == DebugInfo::kNoDebuggingId) {
    id = isolate_->heap()->NextDebuggingId();
    debug_info->set_debugging_id(id);
  }
  return id;
}

bool Debug::SetBreakpointForFunction(Handle<SharedFunctionInfo> shared,
                                     Handle<String> condition, int* id) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  *id = ++thread_local_.last_breakpoint_id_;
  Handle<BreakPoint> breakpoint =
      isolate_->factory()->NewBreakPoint(*id, condition);
  int source_position = 0;
#if V8_ENABLE_WEBASSEMBLY
  // Handle wasm function.
  if (shared->HasWasmExportedFunctionData()) {
    int func_index = shared->wasm_exported_function_data().function_index();
    Handle<WasmInstanceObject> wasm_instance(
        shared->wasm_exported_function_data().instance(), isolate_);
    Handle<Script> script(Script::cast(wasm_instance->module_object().script()),
                          isolate_);
    return WasmScript::SetBreakPointOnFirstBreakableForFunction(
        script, func_index, breakpoint);
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  return SetBreakpoint(shared, breakpoint, &source_position);
}

void Debug::RemoveBreakpoint(int id) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  Handle<BreakPoint> breakpoint = isolate_->factory()->NewBreakPoint(
      id, isolate_->factory()->empty_string());
  ClearBreakPoint(breakpoint);
}

#if V8_ENABLE_WEBASSEMBLY
void Debug::SetOnEntryBreakpointForWasmScript(Handle<Script> script, int* id) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK_EQ(Script::TYPE_WASM, script->type());
  *id = ++thread_local_.last_breakpoint_id_;
  Handle<BreakPoint> break_point = isolate_->factory()->NewBreakPoint(
      *id, isolate_->factory()->empty_string());
  RecordWasmScriptWithBreakpoints(script);
  WasmScript::SetBreakPointOnEntry(script, break_point);
}

void Debug::RemoveBreakpointForWasmScript(Handle<Script> script, int id) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (script->type() == Script::TYPE_WASM) {
    WasmScript::ClearBreakPointById(script, id);
  }
}

void Debug::RecordWasmScriptWithBreakpoints(Handle<Script> script) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (wasm_scripts_with_breakpoints_.is_null()) {
    Handle<WeakArrayList> new_list = isolate_->factory()->NewWeakArrayList(4);
    wasm_scripts_with_breakpoints_ =
        isolate_->global_handles()->Create(*new_list);
  }
  {
    DisallowGarbageCollection no_gc;
    for (int idx = wasm_scripts_with_breakpoints_->length() - 1; idx >= 0;
         --idx) {
      HeapObject wasm_script;
      if (wasm_scripts_with_breakpoints_->Get(idx).GetHeapObject(
              &wasm_script) &&
          wasm_script == *script) {
        return;
      }
    }
  }
  Handle<WeakArrayList> new_list = WeakArrayList::Append(
      isolate_, wasm_scripts_with_breakpoints_, MaybeObjectHandle{script});
  if (*new_list != *wasm_scripts_with_breakpoints_) {
    isolate_->global_handles()->Destroy(
        wasm_scripts_with_breakpoints_.location());
    wasm_scripts_with_breakpoints_ =
        isolate_->global_handles()->Create(*new_list);
  }
}
#endif  // V8_ENABLE_WEBASSEMBLY

// Clear out all the debug break code.
void Debug::ClearAllBreakPoints() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  ClearAllDebugInfos([=](Handle<DebugInfo> info) {
    ClearBreakPoints(info);
    info->ClearBreakInfo(isolate_);
  });
#if V8_ENABLE_WEBASSEMBLY
  // Clear all wasm breakpoints.
  if (!wasm_scripts_with_breakpoints_.is_null()) {
    DisallowGarbageCollection no_gc;
    for (int idx = wasm_scripts_with_breakpoints_->length() - 1; idx >= 0;
         --idx) {
      HeapObject raw_wasm_script;
      if (wasm_scripts_with_breakpoints_->Get(idx).GetHeapObject(
              &raw_wasm_script)) {
        Script wasm_script = Script::cast(raw_wasm_script);
        WasmScript::ClearAllBreakpoints(wasm_script);
        wasm_script.wasm_native_module()->GetDebugInfo()->RemoveIsolate(
            isolate_);
      }
    }
    wasm_scripts_with_breakpoints_ = Handle<WeakArrayList>{};
  }
#endif  // V8_ENABLE_WEBASSEMBLY
}

void Debug::FloodWithOneShot(Handle<SharedFunctionInfo> shared,
                             bool returns_only) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (IsBlackboxed(shared)) return;
  // Make sure the function is compiled and has set up the debug info.
  if (!EnsureBreakInfo(shared)) return;
  PrepareFunctionForDebugExecution(shared);

  Handle<DebugInfo> debug_info(shared->GetDebugInfo(), isolate_);
  // Flood the function with break points.
  DCHECK(debug_info->HasInstrumentedBytecodeArray());
  for (BreakIterator it(debug_info); !it.Done(); it.Next()) {
    if (returns_only && !it.GetBreakLocation().IsReturnOrSuspend()) continue;
    it.SetDebugBreak();
  }
}

void Debug::ChangeBreakOnException(ExceptionBreakType type, bool enable) {
  if (type == BreakUncaughtException) {
    break_on_uncaught_exception_ = enable;
  } else {
    break_on_exception_ = enable;
  }
}

bool Debug::IsBreakOnException(ExceptionBreakType type) {
  if (type == BreakUncaughtException) {
    return break_on_uncaught_exception_;
  } else {
    return break_on_exception_;
  }
}

MaybeHandle<FixedArray> Debug::GetHitBreakPoints(Handle<DebugInfo> debug_info,
                                                 int position) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  Handle<Object> break_points = debug_info->GetBreakPoints(isolate_, position);
  bool is_break_at_entry = debug_info->BreakAtEntry();
  DCHECK(!break_points->IsUndefined(isolate_));
  if (!break_points->IsFixedArray()) {
    if (!CheckBreakPoint(Handle<BreakPoint>::cast(break_points),
                         is_break_at_entry)) {
      return {};
    }
    Handle<FixedArray> break_points_hit = isolate_->factory()->NewFixedArray(1);
    break_points_hit->set(0, *break_points);
    return break_points_hit;
  }

  Handle<FixedArray> array(FixedArray::cast(*break_points), isolate_);
  int num_objects = array->length();
  Handle<FixedArray> break_points_hit =
      isolate_->factory()->NewFixedArray(num_objects);
  int break_points_hit_count = 0;
  for (int i = 0; i < num_objects; ++i) {
    Handle<Object> break_point(array->get(i), isolate_);
    if (CheckBreakPoint(Handle<BreakPoint>::cast(break_point),
                        is_break_at_entry)) {
      break_points_hit->set(break_points_hit_count++, *break_point);
    }
  }
  if (break_points_hit_count == 0) return {};
  break_points_hit->Shrink(isolate_, break_points_hit_count);
  return break_points_hit;
}

void Debug::SetBreakOnNextFunctionCall() {
  // This method forces V8 to break on next function call regardless current
  // last_step_action_. If any break happens between SetBreakOnNextFunctionCall
  // and ClearBreakOnNextFunctionCall, we will clear this flag and stepping. If
  // break does not happen, e.g. all called functions are blackboxed or no
  // function is called, then we will clear this flag and let stepping continue
  // its normal business.
  thread_local_.break_on_next_function_call_ = true;
  UpdateHookOnFunctionCall();
}

void Debug::ClearBreakOnNextFunctionCall() {
  thread_local_.break_on_next_function_call_ = false;
  UpdateHookOnFunctionCall();
}

void Debug::PrepareStepIn(Handle<JSFunction> function) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  CHECK(last_step_action() >= StepInto || break_on_next_function_call());
  if (ignore_events()) return;
  if (in_debug_scope()) return;
  if (break_disabled()) return;
  Handle<SharedFunctionInfo> shared(function->shared(), isolate_);
  if (IsBlackboxed(shared)) return;
  if (*function == thread_local_.ignore_step_into_function_) return;
  thread_local_.ignore_step_into_function_ = Smi::zero();
  FloodWithOneShot(Handle<SharedFunctionInfo>(function->shared(), isolate_));
}

void Debug::PrepareStepInSuspendedGenerator() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  CHECK(has_suspended_generator());
  if (ignore_events()) return;
  if (in_debug_scope()) return;
  if (break_disabled()) return;
  thread_local_.last_step_action_ = StepInto;
  UpdateHookOnFunctionCall();
  Handle<JSFunction> function(
      JSGeneratorObject::cast(thread_local_.suspended_generator_).function(),
      isolate_);
  FloodWithOneShot(Handle<SharedFunctionInfo>(function->shared(), isolate_));
  clear_suspended_generator();
}

void Debug::PrepareStepOnThrow() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (last_step_action() == StepNone) return;
  if (ignore_events()) return;
  if (in_debug_scope()) return;
  if (break_disabled()) return;

  ClearOneShot();

  int current_frame_count = CurrentFrameCount();

  // Iterate through the JavaScript stack looking for handlers.
  JavaScriptFrameIterator it(isolate_);
  while (!it.done()) {
    JavaScriptFrame* frame = it.frame();
    if (frame->LookupExceptionHandlerInTable(nullptr, nullptr) > 0) break;
    std::vector<SharedFunctionInfo> infos;
    frame->GetFunctions(&infos);
    current_frame_count -= infos.size();
    it.Advance();
  }

  // No handler found. Nothing to instrument.
  if (it.done()) return;

  bool found_handler = false;
  // Iterate frames, including inlined frames. First, find the handler frame.
  // Then skip to the frame we want to break in, then instrument for stepping.
  for (; !it.done(); it.Advance()) {
    JavaScriptFrame* frame = JavaScriptFrame::cast(it.frame());
    if (last_step_action() == StepInto) {
      // Deoptimize frame to ensure calls are checked for step-in.
      Deoptimizer::DeoptimizeFunction(frame->function());
    }
    std::vector<FrameSummary> summaries;
    frame->Summarize(&summaries);
    for (size_t i = summaries.size(); i != 0; i--, current_frame_count--) {
      const FrameSummary& summary = summaries[i - 1];
      if (!found_handler) {
        // We have yet to find the handler. If the frame inlines multiple
        // functions, we have to check each one for the handler.
        // If it only contains one function, we already found the handler.
        if (summaries.size() > 1) {
          Handle<AbstractCode> code = summary.AsJavaScript().abstract_code();
          CHECK_EQ(CodeKind::INTERPRETED_FUNCTION, code->kind());
          HandlerTable table(code->GetBytecodeArray());
          int code_offset = summary.code_offset();
          HandlerTable::CatchPrediction prediction;
          int index = table.LookupRange(code_offset, nullptr, &prediction);
          if (index > 0) found_handler = true;
        } else {
          found_handler = true;
        }
      }

      if (found_handler) {
        // We found the handler. If we are stepping next or out, we need to
        // iterate until we found the suitable target frame to break in.
        if ((last_step_action() == StepOver || last_step_action() == StepOut) &&
            current_frame_count > thread_local_.target_frame_count_) {
          continue;
        }
        Handle<SharedFunctionInfo> info(
            summary.AsJavaScript().function()->shared(), isolate_);
        if (IsBlackboxed(info)) continue;
        FloodWithOneShot(info);
        return;
      }
    }
  }
}

void Debug::PrepareStep(StepAction step_action) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);

  DCHECK(in_debug_scope());

  // Get the frame where the execution has stopped and skip the debug frame if
  // any. The debug frame will only be present if execution was stopped due to
  // hitting a break point. In other situations (e.g. unhandled exception) the
  // debug frame is not present.
  StackFrameId frame_id = break_frame_id();
  // If there is no JavaScript stack don't do anything.
  if (frame_id == StackFrameId::NO_ID) return;

  feature_tracker()->Track(DebugFeatureTracker::kStepping);

  thread_local_.last_step_action_ = step_action;

  StackTraceFrameIterator frames_it(isolate_, frame_id);
  CommonFrame* frame = frames_it.frame();

  BreakLocation location = BreakLocation::Invalid();
  Handle<SharedFunctionInfo> shared;
  int current_frame_count = CurrentFrameCount();

  if (frame->is_java_script()) {
    JavaScriptFrame* js_frame = JavaScriptFrame::cast(frame);
    DCHECK(js_frame->function().IsJSFunction());

    // Get the debug info (create it if it does not exist).
    auto summary = FrameSummary::GetTop(frame).AsJavaScript();
    Handle<JSFunction> function(summary.function());
    shared = Handle<SharedFunctionInfo>(function->shared(), isolate_);
    if (!EnsureBreakInfo(shared)) return;
    PrepareFunctionForDebugExecution(shared);

    // PrepareFunctionForDebugExecution can invalidate Baseline frames
    js_frame = JavaScriptFrame::cast(frames_it.Reframe());

    Handle<DebugInfo> debug_info(shared->GetDebugInfo(), isolate_);
    location = BreakLocation::FromFrame(debug_info, js_frame);

    // Any step at a return is a step-out, and a step-out at a suspend behaves
    // like a return.
    if (location.IsReturn() ||
        (location.IsSuspend() &&
         (step_action == StepOut || (IsGeneratorFunction(shared->kind()) &&
                                     location.generator_suspend_id() == 0)))) {
      // On StepOut we'll ignore our further calls to current function in
      // PrepareStepIn callback.
      if (last_step_action() == StepOut) {
        thread_local_.ignore_step_into_function_ = *function;
      }
      step_action = StepOut;
      thread_local_.last_step_action_ = StepInto;
    }

    // We need to schedule DebugOnFunction call callback
    UpdateHookOnFunctionCall();

    // A step-next in blackboxed function is a step-out.
    if (step_action == StepOver && IsBlackboxed(shared)) step_action = StepOut;

    thread_local_.last_statement_position_ =
        summary.abstract_code()->SourceStatementPosition(summary.code_offset());
    thread_local_.last_frame_count_ = current_frame_count;
    // No longer perform the current async step.
    clear_suspended_generator();
#if V8_ENABLE_WEBASSEMBLY
  } else if (frame->is_wasm() && step_action != StepOut) {
    // Handle stepping in wasm.
    WasmFrame* wasm_frame = WasmFrame::cast(frame);
    auto* debug_info = wasm_frame->native_module()->GetDebugInfo();
    if (debug_info->PrepareStep(wasm_frame)) {
      UpdateHookOnFunctionCall();
      return;
    }
    // If the wasm code is not debuggable or will return after this step
    // (indicated by {PrepareStep} returning false), then step out of that frame
    // instead.
    step_action = StepOut;
    UpdateHookOnFunctionCall();
#endif  // V8_ENABLE_WEBASSEMBLY
  }

  switch (step_action) {
    case StepNone:
      UNREACHABLE();
    case StepOut: {
      // Clear last position info. For stepping out it does not matter.
      thread_local_.last_statement_position_ = kNoSourcePosition;
      thread_local_.last_frame_count_ = -1;
      if (!shared.is_null() && !location.IsReturnOrSuspend() &&
          !IsBlackboxed(shared)) {
        // At not return position we flood return positions with one shots and
        // will repeat StepOut automatically at next break.
        thread_local_.target_frame_count_ = current_frame_count;
        thread_local_.fast_forward_to_return_ = true;
        FloodWithOneShot(shared, true);
        return;
      }
      // Skip the current frame, find the first frame we want to step out to
      // and deoptimize every frame along the way.
      bool in_current_frame = true;
      for (; !frames_it.done(); frames_it.Advance()) {
#if V8_ENABLE_WEBASSEMBLY
        if (frames_it.frame()->is_wasm()) {
          if (in_current_frame) {
            in_current_frame = false;
            continue;
          }
          // Handle stepping out into Wasm.
          WasmFrame* wasm_frame = WasmFrame::cast(frames_it.frame());
          auto* debug_info = wasm_frame->native_module()->GetDebugInfo();
          debug_info->PrepareStepOutTo(wasm_frame);
          return;
        }
#endif  // V8_ENABLE_WEBASSEMBLY
        JavaScriptFrame* frame = JavaScriptFrame::cast(frames_it.frame());
        if (last_step_action() == StepInto) {
          // Deoptimize frame to ensure calls are checked for step-in.
          Deoptimizer::DeoptimizeFunction(frame->function());
        }
        HandleScope scope(isolate_);
        std::vector<Handle<SharedFunctionInfo>> infos;
        frame->GetFunctions(&infos);
        for (; !infos.empty(); current_frame_count--) {
          Handle<SharedFunctionInfo> info = infos.back();
          infos.pop_back();
          if (in_current_frame) {
            // We want to step out, so skip the current frame.
            in_current_frame = false;
            continue;
          }
          if (IsBlackboxed(info)) continue;
          FloodWithOneShot(info);
          thread_local_.target_frame_count_ = current_frame_count;
          return;
        }
      }
      break;
    }
    case StepOver:
      thread_local_.target_frame_count_ = current_frame_count;
      V8_FALLTHROUGH;
    case StepInto:
      FloodWithOneShot(shared);
      break;
  }
}

// Simple function for returning the source positions for active break points.
Handle<Object> Debug::GetSourceBreakLocations(
    Isolate* isolate, Handle<SharedFunctionInfo> shared) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kDebugger);
  if (!shared->HasBreakInfo()) {
    return isolate->factory()->undefined_value();
  }

  Handle<DebugInfo> debug_info(shared->GetDebugInfo(), isolate);
  if (debug_info->GetBreakPointCount(isolate) == 0) {
    return isolate->factory()->undefined_value();
  }
  Handle<FixedArray> locations = isolate->factory()->NewFixedArray(
      debug_info->GetBreakPointCount(isolate));
  int count = 0;
  for (int i = 0; i < debug_info->break_points().length(); ++i) {
    if (!debug_info->break_points().get(i).IsUndefined(isolate)) {
      BreakPointInfo break_point_info =
          BreakPointInfo::cast(debug_info->break_points().get(i));
      int break_points = break_point_info.GetBreakPointCount(isolate);
      if (break_points == 0) continue;
      for (int j = 0; j < break_points; ++j) {
        locations->set(count++,
                       Smi::FromInt(break_point_info.source_position()));
      }
    }
  }
  return locations;
}

void Debug::ClearStepping() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Clear the various stepping setup.
  ClearOneShot();

  thread_local_.last_step_action_ = StepNone;
  thread_local_.last_statement_position_ = kNoSourcePosition;
  thread_local_.ignore_step_into_function_ = Smi::zero();
  thread_local_.fast_forward_to_return_ = false;
  thread_local_.last_frame_count_ = -1;
  thread_local_.target_frame_count_ = -1;
  thread_local_.break_on_next_function_call_ = false;
  UpdateHookOnFunctionCall();
}

// Clears all the one-shot break points that are currently set. Normally this
// function is called each time a break point is hit as one shot break points
// are used to support stepping.
void Debug::ClearOneShot() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // The current implementation just runs through all the breakpoints. When the
  // last break point for a function is removed that function is automatically
  // removed from the list.
  for (DebugInfoListNode* node = debug_info_list_; node != nullptr;
       node = node->next()) {
    Handle<DebugInfo> debug_info = node->debug_info();
    ClearBreakPoints(debug_info);
    ApplyBreakPoints(debug_info);
  }
}

namespace {
class DiscardBaselineCodeVisitor : public ThreadVisitor {
 public:
  explicit DiscardBaselineCodeVisitor(SharedFunctionInfo shared)
      : shared_(shared) {}
  DiscardBaselineCodeVisitor() : shared_(SharedFunctionInfo()) {}

  void VisitThread(Isolate* isolate, ThreadLocalTop* top) override {
    bool deopt_all = shared_ == SharedFunctionInfo();
    for (JavaScriptFrameIterator it(isolate, top); !it.done(); it.Advance()) {
      if (!deopt_all && it.frame()->function().shared() != shared_) continue;
      if (it.frame()->type() == StackFrame::BASELINE) {
        BaselineFrame* frame = BaselineFrame::cast(it.frame());
        int bytecode_offset = frame->GetBytecodeOffset();
        Address* pc_addr = frame->pc_address();
        Address advance = BUILTIN_CODE(isolate, InterpreterEnterAtNextBytecode)
                              ->InstructionStart();
        PointerAuthentication::ReplacePC(pc_addr, advance, kSystemPointerSize);
        InterpretedFrame::cast(it.Reframe())
            ->PatchBytecodeOffset(bytecode_offset);
      } else if (it.frame()->type() == StackFrame::INTERPRETED) {
        // Check if the PC is a baseline entry trampoline. If it is, replace it
        // with the corresponding interpreter entry trampoline.
        // This is the case if a baseline function was inlined into a function
        // we deoptimized in the debugger and are stepping into it.
        JavaScriptFrame* frame = it.frame();
        Address pc = frame->pc();
        Builtin builtin = InstructionStream::TryLookupCode(isolate, pc);
        if (builtin == Builtin::kBaselineOrInterpreterEnterAtBytecode ||
            builtin == Builtin::kBaselineOrInterpreterEnterAtNextBytecode) {
          Address* pc_addr = frame->pc_address();
          Builtin advance =
              builtin == Builtin::kBaselineOrInterpreterEnterAtBytecode
                  ? Builtin::kInterpreterEnterAtBytecode
                  : Builtin::kInterpreterEnterAtNextBytecode;
          Address advance_pc =
              isolate->builtins()->code(advance).InstructionStart();
          PointerAuthentication::ReplacePC(pc_addr, advance_pc,
                                           kSystemPointerSize);
        }
      }
    }
  }

 private:
  SharedFunctionInfo shared_;
  DISALLOW_GARBAGE_COLLECTION(no_gc_)
};
}  // namespace

void Debug::DiscardBaselineCode(SharedFunctionInfo shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK(shared.HasBaselineData());
  Isolate* isolate = shared.GetIsolate();
  DiscardBaselineCodeVisitor visitor(shared);
  visitor.VisitThread(isolate, isolate->thread_local_top());
  isolate->thread_manager()->IterateArchivedThreads(&visitor);
  // TODO(v8:11429): Avoid this heap walk somehow.
  HeapObjectIterator iterator(isolate->heap());
  auto trampoline = BUILTIN_CODE(isolate, InterpreterEntryTrampoline);
  shared.flush_baseline_data();
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    if (obj.IsJSFunction()) {
      JSFunction fun = JSFunction::cast(obj);
      if (fun.shared() == shared && fun.ActiveTierIsBaseline()) {
        fun.set_code(*trampoline);
      }
    }
  }
}

void Debug::DiscardAllBaselineCode() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DiscardBaselineCodeVisitor visitor;
  visitor.VisitThread(isolate_, isolate_->thread_local_top());
  HeapObjectIterator iterator(isolate_->heap());
  auto trampoline = BUILTIN_CODE(isolate_, InterpreterEntryTrampoline);
  isolate_->thread_manager()->IterateArchivedThreads(&visitor);
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    if (obj.IsJSFunction()) {
      JSFunction fun = JSFunction::cast(obj);
      if (fun.shared().HasBaselineData()) {
        fun.set_code(*trampoline);
      }
    }
  }
}

void Debug::DeoptimizeFunction(Handle<SharedFunctionInfo> shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Deoptimize all code compiled from this shared function info including
  // inlining.
  isolate_->AbortConcurrentOptimization(BlockingBehavior::kBlock);

  if (shared->HasBaselineData()) {
    DiscardBaselineCode(*shared);
  }

  bool found_something = false;
  Code::OptimizedCodeIterator iterator(isolate_);
  do {
    Code code = iterator.Next();
    if (code.is_null()) break;
    if (code.Inlines(*shared)) {
      code.set_marked_for_deoptimization(true);
      found_something = true;
    }
  } while (true);

  if (found_something) {
    // Only go through with the deoptimization if something was found.
    Deoptimizer::DeoptimizeMarkedCode(isolate_);
  }
}

void Debug::PrepareFunctionForDebugExecution(
    Handle<SharedFunctionInfo> shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // To prepare bytecode for debugging, we already need to have the debug
  // info (containing the debug copy) upfront, but since we do not recompile,
  // preparing for break points cannot fail.
  DCHECK(shared->is_compiled());
  DCHECK(shared->HasDebugInfo());
  Handle<DebugInfo> debug_info = GetOrCreateDebugInfo(shared);
  if (debug_info->flags(kRelaxedLoad) & DebugInfo::kPreparedForDebugExecution)
    return;

  if (shared->HasBytecodeArray()) {
    SharedFunctionInfo::InstallDebugBytecode(shared, isolate_);
  }

  if (debug_info->CanBreakAtEntry()) {
    // Deopt everything in case the function is inlined anywhere.
    Deoptimizer::DeoptimizeAll(isolate_);
    DiscardAllBaselineCode();
    InstallDebugBreakTrampoline();
  } else {
    DeoptimizeFunction(shared);
    // Update PCs on the stack to point to recompiled code.
    RedirectActiveFunctions redirect_visitor(
        *shared, RedirectActiveFunctions::Mode::kUseDebugBytecode);
    redirect_visitor.VisitThread(isolate_, isolate_->thread_local_top());
    isolate_->thread_manager()->IterateArchivedThreads(&redirect_visitor);
  }
  debug_info->set_flags(
      debug_info->flags(kRelaxedLoad) | DebugInfo::kPreparedForDebugExecution,
      kRelaxedStore);
}

void Debug::InstallDebugBreakTrampoline() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Check the list of debug infos whether the debug break trampoline needs to
  // be installed. If that's the case, iterate the heap for functions to rewire
  // to the trampoline.
  HandleScope scope(isolate_);
  // If there is a breakpoint at function entry, we need to install trampoline.
  bool needs_to_use_trampoline = false;
  // If there we break at entry to an api callback, we need to clear ICs.
  bool needs_to_clear_ic = false;
  for (DebugInfoListNode* current = debug_info_list_; current != nullptr;
       current = current->next()) {
    if (current->debug_info()->CanBreakAtEntry()) {
      needs_to_use_trampoline = true;
      if (current->debug_info()->shared().IsApiFunction()) {
        needs_to_clear_ic = true;
        break;
      }
    }
  }

  if (!needs_to_use_trampoline) return;

  Handle<Code> trampoline = BUILTIN_CODE(isolate_, DebugBreakTrampoline);
  std::vector<Handle<JSFunction>> needs_compile;
  {
    HeapObjectIterator iterator(isolate_->heap());
    for (HeapObject obj = iterator.Next(); !obj.is_null();
         obj = iterator.Next()) {
      if (needs_to_clear_ic && obj.IsFeedbackVector()) {
        FeedbackVector::cast(obj).ClearSlots(isolate_);
        continue;
      } else if (obj.IsJSFunction()) {
        JSFunction fun = JSFunction::cast(obj);
        SharedFunctionInfo shared = fun.shared();
        if (!shared.HasDebugInfo()) continue;
        if (!shared.GetDebugInfo().CanBreakAtEntry()) continue;
        if (!fun.is_compiled()) {
          needs_compile.push_back(handle(fun, isolate_));
        } else {
          fun.set_code(*trampoline);
        }
      }
    }
  }

  // By overwriting the function code with DebugBreakTrampoline, which tailcalls
  // to shared code, we bypass CompileLazy. Perform CompileLazy here instead.
  for (Handle<JSFunction> fun : needs_compile) {
    IsCompiledScope is_compiled_scope;
    Compiler::Compile(isolate_, fun, Compiler::CLEAR_EXCEPTION,
                      &is_compiled_scope);
    DCHECK(is_compiled_scope.is_compiled());
    fun->set_code(*trampoline);
  }
}

namespace {
template <typename Iterator>
void GetBreakablePositions(Iterator* it, int start_position, int end_position,
                           std::vector<BreakLocation>* locations) {
  while (!it->Done()) {
    if (it->position() >= start_position && it->position() < end_position) {
      locations->push_back(it->GetBreakLocation());
    }
    it->Next();
  }
}

void FindBreakablePositions(Handle<DebugInfo> debug_info, int start_position,
                            int end_position,
                            std::vector<BreakLocation>* locations) {
  DCHECK(debug_info->HasInstrumentedBytecodeArray());
  BreakIterator it(debug_info);
  GetBreakablePositions(&it, start_position, end_position, locations);
}

bool CompileTopLevel(Isolate* isolate, Handle<Script> script) {
  UnoptimizedCompileState compile_state(isolate);
  UnoptimizedCompileFlags flags =
      UnoptimizedCompileFlags::ForScriptCompile(isolate, *script);
  ParseInfo parse_info(isolate, flags, &compile_state);
  IsCompiledScope is_compiled_scope;
  const MaybeHandle<SharedFunctionInfo> maybe_result =
      Compiler::CompileToplevel(&parse_info, script, isolate,
                                &is_compiled_scope);
  if (maybe_result.is_null()) {
    if (isolate->has_pending_exception()) {
      isolate->clear_pending_exception();
    }
    return false;
  }
  return true;
}
}  // namespace

bool Debug::GetPossibleBreakpoints(Handle<Script> script, int start_position,
                                   int end_position, bool restrict_to_function,
                                   std::vector<BreakLocation>* locations) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (restrict_to_function) {
    Handle<Object> result =
        FindInnermostContainingFunctionInfo(script, start_position);
    if (result->IsUndefined(isolate_)) return false;

    // Make sure the function has set up the debug info.
    Handle<SharedFunctionInfo> shared =
        Handle<SharedFunctionInfo>::cast(result);
    if (!EnsureBreakInfo(shared)) return false;
    PrepareFunctionForDebugExecution(shared);

    Handle<DebugInfo> debug_info(shared->GetDebugInfo(), isolate_);
    FindBreakablePositions(debug_info, start_position, end_position, locations);
    return true;
  }

  HandleScope scope(isolate_);
  std::vector<Handle<SharedFunctionInfo>> candidates;
  if (!FindSharedFunctionInfosIntersectingRange(script, start_position,
                                                end_position, &candidates)) {
    return false;
  }
  for (const auto& candidate : candidates) {
    CHECK(candidate->HasBreakInfo());
    Handle<DebugInfo> debug_info(candidate->GetDebugInfo(), isolate_);
    FindBreakablePositions(debug_info, start_position, end_position, locations);
  }
  return true;
}

class SharedFunctionInfoFinder {
 public:
  explicit SharedFunctionInfoFinder(int target_position)
      : current_start_position_(kNoSourcePosition),
        target_position_(target_position) {}

  void NewCandidate(SharedFunctionInfo shared,
                    JSFunction closure = JSFunction()) {
    if (!shared.IsSubjectToDebugging()) return;
    int start_position = shared.function_token_position();
    if (start_position == kNoSourcePosition) {
      start_position = shared.StartPosition();
    }

    if (start_position > target_position_) return;
    if (target_position_ > shared.EndPosition()) return;

    if (!current_candidate_.is_null()) {
      if (current_start_position_ == start_position &&
          shared.EndPosition() == current_candidate_.EndPosition()) {
        // If we already have a matching closure, do not throw it away.
        if (!current_candidate_closure_.is_null() && closure.is_null()) return;
        // If a top-level function contains only one function
        // declaration the source for the top-level and the function
        // is the same. In that case prefer the non top-level function.
        if (!current_candidate_.is_toplevel() && shared.is_toplevel()) return;
      } else if (start_position < current_start_position_ ||
                 current_candidate_.EndPosition() < shared.EndPosition()) {
        return;
      }
    }

    current_start_position_ = start_position;
    current_candidate_ = shared;
    current_candidate_closure_ = closure;
  }

  SharedFunctionInfo Result() { return current_candidate_; }

  JSFunction ResultClosure() { return current_candidate_closure_; }

 private:
  SharedFunctionInfo current_candidate_;
  JSFunction current_candidate_closure_;
  int current_start_position_;
  int target_position_;
  DISALLOW_GARBAGE_COLLECTION(no_gc_)
};

namespace {
SharedFunctionInfo FindSharedFunctionInfoCandidate(int position,
                                                   Handle<Script> script,
                                                   Isolate* isolate) {
  SharedFunctionInfoFinder finder(position);
  SharedFunctionInfo::ScriptIterator iterator(isolate, *script);
  for (SharedFunctionInfo info = iterator.Next(); !info.is_null();
       info = iterator.Next()) {
    finder.NewCandidate(info);
  }
  return finder.Result();
}
}  // namespace

Handle<SharedFunctionInfo> Debug::FindClosestSharedFunctionInfoFromPosition(
    int position, Handle<Script> script,
    Handle<SharedFunctionInfo> outer_shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  CHECK(outer_shared->HasBreakInfo());
  int closest_position = FindBreakablePosition(
      Handle<DebugInfo>(outer_shared->GetDebugInfo(), isolate_), position);
  Handle<SharedFunctionInfo> closest_candidate = outer_shared;
  if (closest_position == position) return outer_shared;

  const int start_position = outer_shared->StartPosition();
  const int end_position = outer_shared->EndPosition();
  if (start_position == end_position) return outer_shared;

  if (closest_position == 0) closest_position = end_position;
  std::vector<Handle<SharedFunctionInfo>> candidates;
  // Find all shared function infos of functions that are intersecting from
  // the requested position until the end of the enclosing function.
  if (!FindSharedFunctionInfosIntersectingRange(
          script, position, closest_position, &candidates)) {
    return outer_shared;
  }

  for (auto candidate : candidates) {
    CHECK(candidate->HasBreakInfo());
    Handle<DebugInfo> debug_info(candidate->GetDebugInfo(), isolate_);
    const int candidate_position = FindBreakablePosition(debug_info, position);
    if (candidate_position >= position &&
        candidate_position < closest_position) {
      closest_position = candidate_position;
      closest_candidate = candidate;
    }
    if (closest_position == position) break;
  }
  return closest_candidate;
}

bool Debug::FindSharedFunctionInfosIntersectingRange(
    Handle<Script> script, int start_position, int end_position,
    std::vector<Handle<SharedFunctionInfo>>* intersecting_shared) {
  bool candidateSubsumesRange = false;
  bool triedTopLevelCompile = false;

  while (true) {
    std::vector<Handle<SharedFunctionInfo>> candidates;
    std::vector<IsCompiledScope> compiled_scopes;
    {
      DisallowGarbageCollection no_gc;
      SharedFunctionInfo::ScriptIterator iterator(isolate_, *script);
      for (SharedFunctionInfo info = iterator.Next(); !info.is_null();
           info = iterator.Next()) {
        if (info.EndPosition() < start_position ||
            info.StartPosition() >= end_position) {
          continue;
        }
        candidateSubsumesRange |= info.StartPosition() <= start_position &&
                                  info.EndPosition() >= end_position;
        if (!info.IsSubjectToDebugging()) continue;
        if (!info.is_compiled() && !info.allows_lazy_compilation()) continue;
        candidates.push_back(i::handle(info, isolate_));
      }
    }

    if (!triedTopLevelCompile && !candidateSubsumesRange &&
        script->shared_function_info_count() > 0) {
      DCHECK_LE(script->shared_function_info_count(),
                script->shared_function_infos().length());
      MaybeObject maybeToplevel = script->shared_function_infos().Get(0);
      HeapObject heap_object;
      const bool topLevelInfoExists =
          maybeToplevel->GetHeapObject(&heap_object) &&
          !heap_object.IsUndefined();
      if (!topLevelInfoExists) {
        triedTopLevelCompile = true;
        const bool success = CompileTopLevel(isolate_, script);
        if (!success) return false;
        continue;
      }
    }

    bool was_compiled = false;
    for (const auto& candidate : candidates) {
      IsCompiledScope is_compiled_scope(candidate->is_compiled_scope(isolate_));
      if (!is_compiled_scope.is_compiled()) {
        // Code that cannot be compiled lazily are internal and not debuggable.
        DCHECK(candidate->allows_lazy_compilation());
        if (!Compiler::Compile(isolate_, candidate, Compiler::CLEAR_EXCEPTION,
                               &is_compiled_scope)) {
          return false;
        } else {
          was_compiled = true;
        }
      }
      DCHECK(is_compiled_scope.is_compiled());
      compiled_scopes.push_back(is_compiled_scope);
      if (!EnsureBreakInfo(candidate)) return false;
      PrepareFunctionForDebugExecution(candidate);
    }
    if (was_compiled) continue;
    *intersecting_shared = std::move(candidates);
    return true;
  }
  UNREACHABLE();
}

// We need to find a SFI for a literal that may not yet have been compiled yet,
// and there may not be a JSFunction referencing it. Find the SFI closest to
// the given position, compile it to reveal possible inner SFIs and repeat.
// While we are at this, also ensure code with debug break slots so that we do
// not have to compile a SFI without JSFunction, which is paifu for those that
// cannot be compiled without context (need to find outer compilable SFI etc.)
Handle<Object> Debug::FindInnermostContainingFunctionInfo(Handle<Script> script,
                                                          int position) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  for (int iteration = 0;; iteration++) {
    // Go through all shared function infos associated with this script to
    // find the innermost function containing this position.
    // If there is no shared function info for this script at all, there is
    // no point in looking for it by walking the heap.

    SharedFunctionInfo shared;
    IsCompiledScope is_compiled_scope;
    {
      shared = FindSharedFunctionInfoCandidate(position, script, isolate_);
      if (shared.is_null()) {
        if (iteration > 0) break;
        // It might be that the shared function info is not available as the
        // top level functions are removed due to the GC. Try to recompile
        // the top level functions.
        const bool success = CompileTopLevel(isolate_, script);
        if (!success) break;
        continue;
      }
      // We found it if it's already compiled.
      is_compiled_scope = shared.is_compiled_scope(isolate_);
      if (is_compiled_scope.is_compiled()) {
        Handle<SharedFunctionInfo> shared_handle(shared, isolate_);
        // If the iteration count is larger than 1, we had to compile the outer
        // function in order to create this shared function info. So there can
        // be no JSFunction referencing it. We can anticipate creating a debug
        // info while bypassing PrepareFunctionForDebugExecution.
        if (iteration > 1) {
          CreateBreakInfo(shared_handle);
        }
        return shared_handle;
      }
    }
    // If not, compile to reveal inner functions.
    HandleScope scope(isolate_);
    // Code that cannot be compiled lazily are internal and not debuggable.
    DCHECK(shared.allows_lazy_compilation());
    if (!Compiler::Compile(isolate_, handle(shared, isolate_),
                           Compiler::CLEAR_EXCEPTION, &is_compiled_scope)) {
      break;
    }
  }
  return isolate_->factory()->undefined_value();
}

// Ensures the debug information is present for shared.
bool Debug::EnsureBreakInfo(Handle<SharedFunctionInfo> shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Return if we already have the break info for shared.
  if (shared->HasBreakInfo()) return true;
  if (!shared->IsSubjectToDebugging() && !CanBreakAtEntry(shared)) {
    return false;
  }
  IsCompiledScope is_compiled_scope = shared->is_compiled_scope(isolate_);
  if (!is_compiled_scope.is_compiled() &&
      !Compiler::Compile(isolate_, shared, Compiler::CLEAR_EXCEPTION,
                         &is_compiled_scope)) {
    return false;
  }
  CreateBreakInfo(shared);
  return true;
}

void Debug::CreateBreakInfo(Handle<SharedFunctionInfo> shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);
  Handle<DebugInfo> debug_info = GetOrCreateDebugInfo(shared);

  // Initialize with break information.

  DCHECK(!debug_info->HasBreakInfo());

  Factory* factory = isolate_->factory();
  Handle<FixedArray> break_points(
      factory->NewFixedArray(DebugInfo::kEstimatedNofBreakPointsInFunction));

  int flags = debug_info->flags(kRelaxedLoad);
  flags |= DebugInfo::kHasBreakInfo;
  if (CanBreakAtEntry(shared)) flags |= DebugInfo::kCanBreakAtEntry;
  debug_info->set_flags(flags, kRelaxedStore);
  debug_info->set_break_points(*break_points);

  SharedFunctionInfo::EnsureSourcePositionsAvailable(isolate_, shared);
}

Handle<DebugInfo> Debug::GetOrCreateDebugInfo(
    Handle<SharedFunctionInfo> shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (shared->HasDebugInfo()) return handle(shared->GetDebugInfo(), isolate_);

  // Create debug info and add it to the list.
  Handle<DebugInfo> debug_info = isolate_->factory()->NewDebugInfo(shared);
  DebugInfoListNode* node = new DebugInfoListNode(isolate_, *debug_info);
  node->set_next(debug_info_list_);
  debug_info_list_ = node;

  return debug_info;
}

void Debug::InstallCoverageInfo(Handle<SharedFunctionInfo> shared,
                                Handle<CoverageInfo> coverage_info) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK(!coverage_info.is_null());

  Handle<DebugInfo> debug_info = GetOrCreateDebugInfo(shared);

  DCHECK(!debug_info->HasCoverageInfo());

  debug_info->set_flags(
      debug_info->flags(kRelaxedLoad) | DebugInfo::kHasCoverageInfo,
      kRelaxedStore);
  debug_info->set_coverage_info(*coverage_info);
}

void Debug::RemoveAllCoverageInfos() {
  ClearAllDebugInfos(
      [=](Handle<DebugInfo> info) { info->ClearCoverageInfo(isolate_); });
}

void Debug::ClearAllDebuggerHints() {
  ClearAllDebugInfos(
      [=](Handle<DebugInfo> info) { info->set_debugger_hints(0); });
}

void Debug::FindDebugInfo(Handle<DebugInfo> debug_info,
                          DebugInfoListNode** prev, DebugInfoListNode** curr) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);
  *prev = nullptr;
  *curr = debug_info_list_;
  while (*curr != nullptr) {
    if ((*curr)->debug_info().is_identical_to(debug_info)) return;
    *prev = *curr;
    *curr = (*curr)->next();
  }

  UNREACHABLE();
}

void Debug::ClearAllDebugInfos(const DebugInfoClearFunction& clear_function) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DebugInfoListNode* prev = nullptr;
  DebugInfoListNode* current = debug_info_list_;
  while (current != nullptr) {
    DebugInfoListNode* next = current->next();
    Handle<DebugInfo> debug_info = current->debug_info();
    clear_function(debug_info);
    if (debug_info->IsEmpty()) {
      FreeDebugInfoListNode(prev, current);
      current = next;
    } else {
      prev = current;
      current = next;
    }
  }
}

void Debug::RemoveBreakInfoAndMaybeFree(Handle<DebugInfo> debug_info) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  debug_info->ClearBreakInfo(isolate_);
  if (debug_info->IsEmpty()) {
    DebugInfoListNode* prev;
    DebugInfoListNode* node;
    FindDebugInfo(debug_info, &prev, &node);
    FreeDebugInfoListNode(prev, node);
  }
}

void Debug::FreeDebugInfoListNode(DebugInfoListNode* prev,
                                  DebugInfoListNode* node) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK(node->debug_info()->IsEmpty());

  // Unlink from list. If prev is nullptr we are looking at the first element.
  if (prev == nullptr) {
    debug_info_list_ = node->next();
  } else {
    prev->set_next(node->next());
  }

  // Pack script back into the
  // SFI::script_or_debug_info field.
  Handle<DebugInfo> debug_info(node->debug_info());
  debug_info->shared().set_script_or_debug_info(debug_info->script(),
                                                kReleaseStore);

  delete node;
}

bool Debug::IsBreakAtReturn(JavaScriptFrame* frame) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);

  // Get the executing function in which the debug break occurred.
  Handle<SharedFunctionInfo> shared(frame->function().shared(), isolate_);

  // With no debug info there are no break points, so we can't be at a return.
  if (!shared->HasBreakInfo()) return false;

  DCHECK(!frame->is_optimized());
  Handle<DebugInfo> debug_info(shared->GetDebugInfo(), isolate_);
  BreakLocation location = BreakLocation::FromFrame(debug_info, frame);
  return location.IsReturn();
}

Handle<FixedArray> Debug::GetLoadedScripts() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  isolate_->heap()->CollectAllGarbage(Heap::kNoGCFlags,
                                      GarbageCollectionReason::kDebugger);
  Factory* factory = isolate_->factory();
  if (!factory->script_list()->IsWeakArrayList()) {
    return factory->empty_fixed_array();
  }
  Handle<WeakArrayList> array =
      Handle<WeakArrayList>::cast(factory->script_list());
  Handle<FixedArray> results = factory->NewFixedArray(array->length());
  int length = 0;
  {
    Script::Iterator iterator(isolate_);
    for (Script script = iterator.Next(); !script.is_null();
         script = iterator.Next()) {
      if (script.HasValidSource()) results->set(length++, script);
    }
  }
  return FixedArray::ShrinkOrEmpty(isolate_, results, length);
}

base::Optional<Object> Debug::OnThrow(Handle<Object> exception) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (in_debug_scope() || ignore_events()) return {};
  // Temporarily clear any scheduled_exception to allow evaluating
  // JavaScript from the debug event handler.
  HandleScope scope(isolate_);
  Handle<Object> scheduled_exception;
  if (isolate_->has_scheduled_exception()) {
    scheduled_exception = handle(isolate_->scheduled_exception(), isolate_);
    isolate_->clear_scheduled_exception();
  }
  Handle<Object> maybe_promise = isolate_->GetPromiseOnStackOnThrow();
  OnException(exception, maybe_promise,
              maybe_promise->IsJSPromise() ? v8::debug::kPromiseRejection
                                           : v8::debug::kException);
  if (!scheduled_exception.is_null()) {
    isolate_->set_scheduled_exception(*scheduled_exception);
  }
  PrepareStepOnThrow();
  // If the OnException handler requested termination, then indicated this to
  // our caller Isolate::Throw so it can deal with it immediatelly instead of
  // throwing the original exception.
  if (isolate_->stack_guard()->CheckTerminateExecution()) {
    isolate_->stack_guard()->ClearTerminateExecution();
    return isolate_->TerminateExecution();
  }
  return {};
}

void Debug::OnPromiseReject(Handle<Object> promise, Handle<Object> value) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (in_debug_scope() || ignore_events()) return;
  HandleScope scope(isolate_);
  // Check whether the promise has been marked as having triggered a message.
  Handle<Symbol> key = isolate_->factory()->promise_debug_marker_symbol();
  if (!promise->IsJSObject() ||
      JSReceiver::GetDataProperty(Handle<JSObject>::cast(promise), key)
          ->IsUndefined(isolate_)) {
    OnException(value, promise, v8::debug::kPromiseRejection);
  }
}

bool Debug::IsExceptionBlackboxed(bool uncaught) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Uncaught exception is blackboxed if all current frames are blackboxed,
  // caught exception if top frame is blackboxed.
  StackTraceFrameIterator it(isolate_);
#if V8_ENABLE_WEBASSEMBLY
  while (!it.done() && it.is_wasm()) it.Advance();
#endif  // V8_ENABLE_WEBASSEMBLY
  bool is_top_frame_blackboxed =
      !it.done() ? IsFrameBlackboxed(it.javascript_frame()) : true;
  if (!uncaught || !is_top_frame_blackboxed) return is_top_frame_blackboxed;
  return AllFramesOnStackAreBlackboxed();
}

bool Debug::IsFrameBlackboxed(JavaScriptFrame* frame) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);
  std::vector<Handle<SharedFunctionInfo>> infos;
  frame->GetFunctions(&infos);
  for (const auto& info : infos) {
    if (!IsBlackboxed(info)) return false;
  }
  return true;
}

void Debug::OnException(Handle<Object> exception, Handle<Object> promise,
                        v8::debug::ExceptionType exception_type) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Do not trigger exception event on stack overflow. We cannot perform
  // anything useful for debugging in that situation.
  StackLimitCheck stack_limit_check(isolate_);
  if (stack_limit_check.JsHasOverflowed()) return;

  // Return if the event has nowhere to go.
  if (!debug_delegate_) return;

  // Return if we are not interested in exception events.
  if (!break_on_exception_ && !break_on_uncaught_exception_) return;

  Isolate::CatchType catch_type = isolate_->PredictExceptionCatcher();

  bool uncaught = catch_type == Isolate::NOT_CAUGHT;
  if (promise->IsJSObject()) {
    Handle<JSObject> jsobject = Handle<JSObject>::cast(promise);
    // Mark the promise as already having triggered a message.
    Handle<Symbol> key = isolate_->factory()->promise_debug_marker_symbol();
    Object::SetProperty(isolate_, jsobject, key, key, StoreOrigin::kMaybeKeyed,
                        Just(ShouldThrow::kThrowOnError))
        .Assert();
    // Check whether the promise reject is considered an uncaught exception.
    if (jsobject->IsJSPromise()) {
      Handle<JSPromise> jspromise = Handle<JSPromise>::cast(jsobject);

      // Ignore the exception if the promise was marked as silent
      if (jspromise->is_silent()) return;

      uncaught = !isolate_->PromiseHasUserDefinedRejectHandler(jspromise);
    } else {
      uncaught = true;
    }
  }

  // Return if the exception is caught and we only care about uncaught
  // exceptions.
  if (!uncaught && !break_on_exception_) {
    DCHECK(break_on_uncaught_exception_);
    return;
  }

  {
    JavaScriptFrameIterator it(isolate_);
    // Check whether the top frame is blackboxed or the break location is muted.
    if (!it.done() && (IsMutedAtCurrentLocation(it.frame()) ||
                       IsExceptionBlackboxed(uncaught))) {
      return;
    }
    if (it.done()) return;  // Do not trigger an event with an empty stack.
  }

  DebugScope debug_scope(this);
  HandleScope scope(isolate_);
  DisableBreak no_recursive_break(this);

  {
    RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebuggerCallback);
    Handle<Context> native_context(isolate_->native_context());
    debug_delegate_->ExceptionThrown(
        v8::Utils::ToLocal(native_context), v8::Utils::ToLocal(exception),
        v8::Utils::ToLocal(promise), uncaught, exception_type);
  }
}

void Debug::OnDebugBreak(Handle<FixedArray> break_points_hit,
                         StepAction lastStepAction) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK(!break_points_hit.is_null());
  // The caller provided for DebugScope.
  AssertDebugContext();
  // Bail out if there is no listener for this event
  if (ignore_events()) return;

#ifdef DEBUG
  PrintBreakLocation();
#endif  // DEBUG

  if (!debug_delegate_) return;
  DCHECK(in_debug_scope());
  HandleScope scope(isolate_);
  DisableBreak no_recursive_break(this);

  if ((lastStepAction == StepAction::StepOver ||
       lastStepAction == StepAction::StepInto) &&
      ShouldBeSkipped()) {
    PrepareStep(lastStepAction);
    return;
  }

  std::vector<int> inspector_break_points_hit;
  int inspector_break_points_count = 0;
  // This array contains breakpoints installed using JS debug API.
  for (int i = 0; i < break_points_hit->length(); ++i) {
    BreakPoint break_point = BreakPoint::cast(break_points_hit->get(i));
    inspector_break_points_hit.push_back(break_point.id());
    ++inspector_break_points_count;
  }
  {
    RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebuggerCallback);
    Handle<Context> native_context(isolate_->native_context());
    debug_delegate_->BreakProgramRequested(v8::Utils::ToLocal(native_context),
                                           inspector_break_points_hit);
  }
}

namespace {
debug::Location GetDebugLocation(Handle<Script> script, int source_position) {
  Script::PositionInfo info;
  Script::GetPositionInfo(script, source_position, &info, Script::WITH_OFFSET);
  // V8 provides ScriptCompiler::CompileFunctionInContext method which takes
  // expression and compile it as anonymous function like (function() ..
  // expression ..). To produce correct locations for stmts inside of this
  // expression V8 compile this function with negative offset. Instead of stmt
  // position blackboxing use function start position which is negative in
  // described case.
  return debug::Location(std::max(info.line, 0), std::max(info.column, 0));
}
}  // namespace

bool Debug::IsBlackboxed(Handle<SharedFunctionInfo> shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  if (!debug_delegate_) return !shared->IsSubjectToDebugging();
  Handle<DebugInfo> debug_info = GetOrCreateDebugInfo(shared);
  if (!debug_info->computed_debug_is_blackboxed()) {
    bool is_blackboxed =
        !shared->IsSubjectToDebugging() || !shared->script().IsScript();
    if (!is_blackboxed) {
      SuppressDebug while_processing(this);
      HandleScope handle_scope(isolate_);
      PostponeInterruptsScope no_interrupts(isolate_);
      DisableBreak no_recursive_break(this);
      DCHECK(shared->script().IsScript());
      Handle<Script> script(Script::cast(shared->script()), isolate_);
      DCHECK(script->IsUserJavaScript());
      debug::Location start = GetDebugLocation(script, shared->StartPosition());
      debug::Location end = GetDebugLocation(script, shared->EndPosition());
      {
        RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebuggerCallback);
        is_blackboxed = debug_delegate_->IsFunctionBlackboxed(
            ToApiHandle<debug::Script>(script), start, end);
      }
    }
    debug_info->set_debug_is_blackboxed(is_blackboxed);
    debug_info->set_computed_debug_is_blackboxed(true);
  }
  return debug_info->debug_is_blackboxed();
}

bool Debug::ShouldBeSkipped() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  SuppressDebug while_processing(this);
  PostponeInterruptsScope no_interrupts(isolate_);
  DisableBreak no_recursive_break(this);

  StackTraceFrameIterator iterator(isolate_);
  CommonFrame* frame = iterator.frame();
  FrameSummary summary = FrameSummary::GetTop(frame);
  Handle<Object> script_obj = summary.script();
  if (!script_obj->IsScript()) return false;

  Handle<Script> script = Handle<Script>::cast(script_obj);
  summary.EnsureSourcePositionsAvailable();
  int source_position = summary.SourcePosition();
  int line = Script::GetLineNumber(script, source_position);
  int column = Script::GetColumnNumber(script, source_position);

  {
    RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebuggerCallback);
    return debug_delegate_->ShouldBeSkipped(ToApiHandle<debug::Script>(script),
                                            line, column);
  }
}

bool Debug::AllFramesOnStackAreBlackboxed() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);
  for (StackTraceFrameIterator it(isolate_); !it.done(); it.Advance()) {
    if (!it.is_javascript()) continue;
    if (!IsFrameBlackboxed(it.javascript_frame())) return false;
  }
  return true;
}

bool Debug::CanBreakAtEntry(Handle<SharedFunctionInfo> shared) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Allow break at entry for builtin functions.
  if (shared->native() || shared->IsApiFunction()) {
    // Functions that are subject to debugging can have regular breakpoints.
    DCHECK(!shared->IsSubjectToDebugging());
    return true;
  }
  return false;
}

bool Debug::SetScriptSource(Handle<Script> script, Handle<String> source,
                            bool preview, debug::LiveEditResult* result) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DebugScope debug_scope(this);
  feature_tracker()->Track(DebugFeatureTracker::kLiveEdit);
  running_live_edit_ = true;
  LiveEdit::PatchScript(isolate_, script, source, preview, result);
  running_live_edit_ = false;
  return result->status == debug::LiveEditResult::OK;
}

void Debug::OnCompileError(Handle<Script> script) {
  ProcessCompileEvent(true, script);
}

void Debug::OnAfterCompile(Handle<Script> script) {
  ProcessCompileEvent(false, script);
}

static void RecordReplayRegisterScript(Handle<Script> script);

void Debug::ProcessCompileEvent(bool has_compile_error, Handle<Script> script) {
  if (!has_compile_error && recordreplay::IsRecordingOrReplaying() && IsMainThread()) {
    RecordReplayRegisterScript(script);
  }

  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Ignore temporary scripts.
  if (script->id() == Script::kTemporaryScriptId) return;
  // TODO(kozyatinskiy): teach devtools to work with liveedit scripts better
  // first and then remove this fast return.
  if (running_live_edit_) return;
  // Attach the correct debug id to the script. The debug id is used by the
  // inspector to filter scripts by native context.
  script->set_context_data(isolate_->native_context()->debug_context_id());
  if (ignore_events()) return;
#if V8_ENABLE_WEBASSEMBLY
  if (!script->IsUserJavaScript() && script->type() != i::Script::TYPE_WASM) {
    return;
  }
#else
  if (!script->IsUserJavaScript()) return;
#endif  // V8_ENABLE_WEBASSEMBLY
  if (!debug_delegate_) return;
  SuppressDebug while_processing(this);
  DebugScope debug_scope(this);
  HandleScope scope(isolate_);
  DisableBreak no_recursive_break(this);
  AllowJavascriptExecution allow_script(isolate_);
  {
    RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebuggerCallback);
    debug_delegate_->ScriptCompiled(ToApiHandle<debug::Script>(script),
                                    running_live_edit_, has_compile_error);
  }
}

int Debug::CurrentFrameCount() {
  StackTraceFrameIterator it(isolate_);
  if (break_frame_id() != StackFrameId::NO_ID) {
    // Skip to break frame.
    DCHECK(in_debug_scope());
    while (!it.done() && it.frame()->id() != break_frame_id()) it.Advance();
  }
  int counter = 0;
  for (; !it.done(); it.Advance()) {
    counter += it.FrameFunctionCount();
  }
  return counter;
}

void Debug::SetDebugDelegate(debug::DebugDelegate* delegate) {
  debug_delegate_ = delegate;
  UpdateState();
}

void Debug::UpdateState() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  bool is_active = debug_delegate_ != nullptr;
  if (is_active == is_active_) return;
  if (is_active) {
    // Note that the debug context could have already been loaded to
    // bootstrap test cases.
    isolate_->compilation_cache()->DisableScriptAndEval();
    isolate_->CollectSourcePositionsForAllBytecodeArrays();
    is_active = true;
    feature_tracker()->Track(DebugFeatureTracker::kActive);
  } else {
    isolate_->compilation_cache()->EnableScriptAndEval();
    Unload();
  }
  is_active_ = is_active;
  isolate_->PromiseHookStateUpdated();
}

void Debug::UpdateHookOnFunctionCall() {
  STATIC_ASSERT(LastStepAction == StepInto);
  hook_on_function_call_ =
      thread_local_.last_step_action_ == StepInto ||
      isolate_->debug_execution_mode() == DebugInfo::kSideEffects ||
      thread_local_.break_on_next_function_call_;
}

void Debug::HandleDebugBreak(IgnoreBreakMode ignore_break_mode) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Ignore debug break during bootstrapping.
  if (isolate_->bootstrapper()->IsActive()) return;
  // Just continue if breaks are disabled.
  if (break_disabled()) return;
  // Ignore debug break if debugger is not active.
  if (!is_active()) return;

  StackLimitCheck check(isolate_);
  if (check.HasOverflowed()) return;

  {
    JavaScriptFrameIterator it(isolate_);
    DCHECK(!it.done());
    Object fun = it.frame()->function();
    if (fun.IsJSFunction()) {
      HandleScope scope(isolate_);
      Handle<JSFunction> function(JSFunction::cast(fun), isolate_);
      // Don't stop in builtin and blackboxed functions.
      Handle<SharedFunctionInfo> shared(function->shared(), isolate_);
      bool ignore_break = ignore_break_mode == kIgnoreIfTopFrameBlackboxed
                              ? IsBlackboxed(shared)
                              : AllFramesOnStackAreBlackboxed();
      if (ignore_break) return;
      // Don't stop if the break location is muted.
      if (IsMutedAtCurrentLocation(it.frame())) return;
    }
  }

  StepAction lastStepAction = last_step_action();

  // Clear stepping to avoid duplicate breaks.
  ClearStepping();

  HandleScope scope(isolate_);
  DebugScope debug_scope(this);

  OnDebugBreak(isolate_->factory()->empty_fixed_array(), lastStepAction);
}

#ifdef DEBUG
void Debug::PrintBreakLocation() {
  if (!FLAG_print_break_location) return;
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  HandleScope scope(isolate_);
  StackTraceFrameIterator iterator(isolate_);
  if (iterator.done()) return;
  CommonFrame* frame = iterator.frame();
  std::vector<FrameSummary> frames;
  frame->Summarize(&frames);
  int inlined_frame_index = static_cast<int>(frames.size() - 1);
  FrameInspector inspector(frame, inlined_frame_index, isolate_);
  int source_position = inspector.GetSourcePosition();
  Handle<Object> script_obj = inspector.GetScript();
  PrintF("[debug] break in function '");
  inspector.GetFunctionName()->PrintOn(stdout);
  PrintF("'.\n");
  if (script_obj->IsScript()) {
    Handle<Script> script = Handle<Script>::cast(script_obj);
    Handle<String> source(String::cast(script->source()), isolate_);
    Script::InitLineEnds(isolate_, script);
    int line =
        Script::GetLineNumber(script, source_position) - script->line_offset();
    int column = Script::GetColumnNumber(script, source_position) -
                 (line == 0 ? script->column_offset() : 0);
    Handle<FixedArray> line_ends(FixedArray::cast(script->line_ends()),
                                 isolate_);
    int line_start = line == 0 ? 0 : Smi::ToInt(line_ends->get(line - 1)) + 1;
    int line_end = Smi::ToInt(line_ends->get(line));
    DisallowGarbageCollection no_gc;
    String::FlatContent content = source->GetFlatContent(no_gc);
    if (content.IsOneByte()) {
      PrintF("[debug] %.*s\n", line_end - line_start,
             content.ToOneByteVector().begin() + line_start);
      PrintF("[debug] ");
      for (int i = 0; i < column; i++) PrintF(" ");
      PrintF("^\n");
    } else {
      PrintF("[debug] at line %d column %d\n", line, column);
    }
  }
}
#endif  // DEBUG

DebugScope::DebugScope(Debug* debug)
    : debug_(debug),
      prev_(reinterpret_cast<DebugScope*>(
          base::Relaxed_Load(&debug->thread_local_.current_debug_scope_))),
      no_interrupts_(debug_->isolate_) {
  // Link recursive debugger entry.
  base::Relaxed_Store(&debug_->thread_local_.current_debug_scope_,
                      reinterpret_cast<base::AtomicWord>(this));
  // Store the previous frame id and return value.
  break_frame_id_ = debug_->break_frame_id();

  // Create the new break info. If there is no proper frames there is no break
  // frame id.
  StackTraceFrameIterator it(isolate());
  bool has_frames = !it.done();
  debug_->thread_local_.break_frame_id_ =
      has_frames ? it.frame()->id() : StackFrameId::NO_ID;

  debug_->UpdateState();
}

void DebugScope::set_terminate_on_resume() { terminate_on_resume_ = true; }

DebugScope::~DebugScope() {
  // Terminate on resume must have been handled by retrieving it, if this is
  // the outer scope.
  if (terminate_on_resume_) {
    if (!prev_) {
      debug_->isolate_->stack_guard()->RequestTerminateExecution();
    } else {
      prev_->set_terminate_on_resume();
    }
  }
  // Leaving this debugger entry.
  base::Relaxed_Store(&debug_->thread_local_.current_debug_scope_,
                      reinterpret_cast<base::AtomicWord>(prev_));

  // Restore to the previous break state.
  debug_->thread_local_.break_frame_id_ = break_frame_id_;

  debug_->UpdateState();
}

ReturnValueScope::ReturnValueScope(Debug* debug) : debug_(debug) {
  return_value_ = debug_->return_value_handle();
}

ReturnValueScope::~ReturnValueScope() {
  debug_->set_return_value(*return_value_);
}

void Debug::UpdateDebugInfosForExecutionMode() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  // Walk all debug infos and update their execution mode if it is different
  // from the isolate execution mode.
  DebugInfoListNode* current = debug_info_list_;
  while (current != nullptr) {
    Handle<DebugInfo> debug_info = current->debug_info();
    if (debug_info->HasInstrumentedBytecodeArray() &&
        debug_info->DebugExecutionMode() != isolate_->debug_execution_mode()) {
      DCHECK(debug_info->shared().HasBytecodeArray());
      if (isolate_->debug_execution_mode() == DebugInfo::kBreakpoints) {
        ClearSideEffectChecks(debug_info);
        ApplyBreakPoints(debug_info);
      } else {
        ClearBreakPoints(debug_info);
        ApplySideEffectChecks(debug_info);
      }
    }
    current = current->next();
  }
}

void Debug::SetTerminateOnResume() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DebugScope* scope = reinterpret_cast<DebugScope*>(
      base::Acquire_Load(&thread_local_.current_debug_scope_));
  CHECK_NOT_NULL(scope);
  scope->set_terminate_on_resume();
}

void Debug::StartSideEffectCheckMode() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK(isolate_->debug_execution_mode() != DebugInfo::kSideEffects);
  isolate_->set_debug_execution_mode(DebugInfo::kSideEffects);
  UpdateHookOnFunctionCall();
  side_effect_check_failed_ = false;

  DCHECK(!temporary_objects_);
  temporary_objects_.reset(new TemporaryObjectsTracker());
  isolate_->heap()->AddHeapObjectAllocationTracker(temporary_objects_.get());
  Handle<FixedArray> array(isolate_->native_context()->regexp_last_match_info(),
                           isolate_);
  regexp_match_info_ =
      Handle<RegExpMatchInfo>::cast(isolate_->factory()->CopyFixedArray(array));

  // Update debug infos to have correct execution mode.
  UpdateDebugInfosForExecutionMode();
}

void Debug::StopSideEffectCheckMode() {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK(isolate_->debug_execution_mode() == DebugInfo::kSideEffects);
  if (side_effect_check_failed_) {
    DCHECK(isolate_->has_pending_exception());
    DCHECK_EQ(ReadOnlyRoots(isolate_).termination_exception(),
              isolate_->pending_exception());
    // Convert the termination exception into a regular exception.
    isolate_->CancelTerminateExecution();
    isolate_->Throw(*isolate_->factory()->NewEvalError(
        MessageTemplate::kNoSideEffectDebugEvaluate));
  }
  isolate_->set_debug_execution_mode(DebugInfo::kBreakpoints);
  UpdateHookOnFunctionCall();
  side_effect_check_failed_ = false;

  DCHECK(temporary_objects_);
  isolate_->heap()->RemoveHeapObjectAllocationTracker(temporary_objects_.get());
  temporary_objects_.reset();
  isolate_->native_context()->set_regexp_last_match_info(*regexp_match_info_);
  regexp_match_info_ = Handle<RegExpMatchInfo>::null();

  // Update debug infos to have correct execution mode.
  UpdateDebugInfosForExecutionMode();
}

void Debug::ApplySideEffectChecks(Handle<DebugInfo> debug_info) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK(debug_info->HasInstrumentedBytecodeArray());
  Handle<BytecodeArray> debug_bytecode(debug_info->DebugBytecodeArray(),
                                       isolate_);
  DebugEvaluate::ApplySideEffectChecks(debug_bytecode);
  debug_info->SetDebugExecutionMode(DebugInfo::kSideEffects);
}

void Debug::ClearSideEffectChecks(Handle<DebugInfo> debug_info) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK(debug_info->HasInstrumentedBytecodeArray());
  Handle<BytecodeArray> debug_bytecode(debug_info->DebugBytecodeArray(),
                                       isolate_);
  Handle<BytecodeArray> original(debug_info->OriginalBytecodeArray(), isolate_);
  for (interpreter::BytecodeArrayIterator it(debug_bytecode); !it.done();
       it.Advance()) {
    // Restore from original. This may copy only the scaling prefix, which is
    // correct, since we patch scaling prefixes to debug breaks if exists.
    debug_bytecode->set(it.current_offset(),
                        original->get(it.current_offset()));
  }
}

bool Debug::PerformSideEffectCheck(Handle<JSFunction> function,
                                   Handle<Object> receiver) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK_EQ(isolate_->debug_execution_mode(), DebugInfo::kSideEffects);
  DisallowJavascriptExecution no_js(isolate_);
  IsCompiledScope is_compiled_scope(
      function->shared().is_compiled_scope(isolate_));
  if (!function->is_compiled() &&
      !Compiler::Compile(isolate_, function, Compiler::KEEP_EXCEPTION,
                         &is_compiled_scope)) {
    return false;
  }
  DCHECK(is_compiled_scope.is_compiled());
  Handle<SharedFunctionInfo> shared(function->shared(), isolate_);
  Handle<DebugInfo> debug_info = GetOrCreateDebugInfo(shared);
  DebugInfo::SideEffectState side_effect_state =
      debug_info->GetSideEffectState(isolate_);
  switch (side_effect_state) {
    case DebugInfo::kHasSideEffects:
      if (FLAG_trace_side_effect_free_debug_evaluate) {
        PrintF("[debug-evaluate] Function %s failed side effect check.\n",
               function->shared().DebugNameCStr().get());
      }
      side_effect_check_failed_ = true;
      // Throw an uncatchable termination exception.
      isolate_->TerminateExecution();
      return false;
    case DebugInfo::kRequiresRuntimeChecks: {
      if (!shared->HasBytecodeArray()) {
        return PerformSideEffectCheckForObject(receiver);
      }
      // If function has bytecode array then prepare function for debug
      // execution to perform runtime side effect checks.
      DCHECK(shared->is_compiled());
      PrepareFunctionForDebugExecution(shared);
      ApplySideEffectChecks(debug_info);
      return true;
    }
    case DebugInfo::kHasNoSideEffect:
      return true;
    case DebugInfo::kNotComputed:
    default:
      UNREACHABLE();
  }
}

Handle<Object> Debug::return_value_handle() {
  return handle(thread_local_.return_value_, isolate_);
}

bool Debug::PerformSideEffectCheckForCallback(
    Handle<Object> callback_info, Handle<Object> receiver,
    Debug::AccessorKind accessor_kind) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK_EQ(!receiver.is_null(), callback_info->IsAccessorInfo());
  DCHECK_EQ(isolate_->debug_execution_mode(), DebugInfo::kSideEffects);
  if (!callback_info.is_null() && callback_info->IsCallHandlerInfo() &&
      i::CallHandlerInfo::cast(*callback_info).NextCallHasNoSideEffect()) {
    return true;
  }
  // TODO(7515): always pass a valid callback info object.
  if (!callback_info.is_null()) {
    if (callback_info->IsAccessorInfo()) {
      // List of allowlisted internal accessors can be found in accessors.h.
      AccessorInfo info = AccessorInfo::cast(*callback_info);
      DCHECK_NE(kNotAccessor, accessor_kind);
      switch (accessor_kind == kSetter ? info.setter_side_effect_type()
                                       : info.getter_side_effect_type()) {
        case SideEffectType::kHasNoSideEffect:
          // We do not support setter accessors with no side effects, since
          // calling set accessors go through a store bytecode. Store bytecodes
          // are considered to cause side effects (to non-temporary objects).
          DCHECK_NE(kSetter, accessor_kind);
          return true;
        case SideEffectType::kHasSideEffectToReceiver:
          DCHECK(!receiver.is_null());
          if (PerformSideEffectCheckForObject(receiver)) return true;
          isolate_->OptionalRescheduleException(false);
          return false;
        case SideEffectType::kHasSideEffect:
          break;
      }
      if (FLAG_trace_side_effect_free_debug_evaluate) {
        PrintF("[debug-evaluate] API Callback '");
        info.name().ShortPrint();
        PrintF("' may cause side effect.\n");
      }
    } else if (callback_info->IsInterceptorInfo()) {
      InterceptorInfo info = InterceptorInfo::cast(*callback_info);
      if (info.has_no_side_effect()) return true;
      if (FLAG_trace_side_effect_free_debug_evaluate) {
        PrintF("[debug-evaluate] API Interceptor may cause side effect.\n");
      }
    } else if (callback_info->IsCallHandlerInfo()) {
      CallHandlerInfo info = CallHandlerInfo::cast(*callback_info);
      if (info.IsSideEffectFreeCallHandlerInfo()) return true;
      if (FLAG_trace_side_effect_free_debug_evaluate) {
        PrintF("[debug-evaluate] API CallHandlerInfo may cause side effect.\n");
      }
    }
  }
  side_effect_check_failed_ = true;
  // Throw an uncatchable termination exception.
  isolate_->TerminateExecution();
  isolate_->OptionalRescheduleException(false);
  return false;
}

bool Debug::PerformSideEffectCheckAtBytecode(InterpretedFrame* frame) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  using interpreter::Bytecode;

  DCHECK_EQ(isolate_->debug_execution_mode(), DebugInfo::kSideEffects);
  SharedFunctionInfo shared = frame->function().shared();
  BytecodeArray bytecode_array = shared.GetBytecodeArray(isolate_);
  int offset = frame->GetBytecodeOffset();
  interpreter::BytecodeArrayIterator bytecode_iterator(
      handle(bytecode_array, isolate_), offset);

  Bytecode bytecode = bytecode_iterator.current_bytecode();
  interpreter::Register reg;
  switch (bytecode) {
    case Bytecode::kStaCurrentContextSlot:
      reg = interpreter::Register::current_context();
      break;
    default:
      reg = bytecode_iterator.GetRegisterOperand(0);
      break;
  }
  Handle<Object> object =
      handle(frame->ReadInterpreterRegister(reg.index()), isolate_);
  return PerformSideEffectCheckForObject(object);
}

bool Debug::PerformSideEffectCheckForObject(Handle<Object> object) {
  RCS_SCOPE(isolate_, RuntimeCallCounterId::kDebugger);
  DCHECK_EQ(isolate_->debug_execution_mode(), DebugInfo::kSideEffects);

  // We expect no side-effects for primitives.
  if (object->IsNumber()) return true;
  if (object->IsName()) return true;

  if (temporary_objects_->HasObject(Handle<HeapObject>::cast(object))) {
    return true;
  }

  if (FLAG_trace_side_effect_free_debug_evaluate) {
    PrintF("[debug-evaluate] failed runtime side effect check.\n");
  }
  side_effect_check_failed_ = true;
  // Throw an uncatchable termination exception.
  isolate_->TerminateExecution();
  return false;
}

// Record Replay handlers and associated helpers. These ought to be in their
// own file, but it's easier to put them here.

////////////////////////////////////////////////////////////////////////////////
// Helpers
////////////////////////////////////////////////////////////////////////////////

extern void RecordReplayOnNewSource(Isolate* isolate, const char* id,
                                    const char* kind, const char* url);

static Handle<String> CStringToHandle(Isolate* isolate, const char* str) {
  base::Vector<const uint8_t> nstr((const uint8_t*) str, strlen(str));
  return isolate->factory()->NewStringFromOneByte(nstr).ToHandleChecked();
}

static Handle<Object> GetProperty(Isolate* isolate,
                                  Handle<Object> obj, const char* property) {
  return Object::GetProperty(isolate, obj, CStringToHandle(isolate, property))
    .ToHandleChecked();
}

static void SetProperty(Isolate* isolate,
                        Handle<Object> obj, const char* property,
                        Handle<Object> value) {
  Object::SetProperty(isolate, obj,
                      CStringToHandle(isolate, property), value).Check();
}

static void SetProperty(Isolate* isolate,
                        Handle<Object> obj, const char* property,
                        const char* value) {
  SetProperty(isolate, obj, property, CStringToHandle(isolate, value));
}

static void SetProperty(Isolate* isolate,
                        Handle<Object> obj, const char* property,
                        double value) {
  SetProperty(isolate, obj, property, isolate->factory()->NewNumber(value));
}

static Handle<JSObject> NewPlainObject(Isolate* isolate) {
  return isolate->factory()->NewJSObject(isolate->object_function());
}

////////////////////////////////////////////////////////////////////////////////
// Script State
////////////////////////////////////////////////////////////////////////////////

// Map ScriptId => Script. We keep all scripts around forever when recording/replaying.
typedef std::unordered_map<int, Eternal<Value>> ScriptIdMap;
static ScriptIdMap* gRecordReplayScripts;

static int GetSourceIdProperty(Isolate* isolate, Handle<Object> obj) {
  Handle<Object> sourceIdStr = GetProperty(isolate, obj, "sourceId");
  std::unique_ptr<char[]> sourceIdText = String::cast(*sourceIdStr).ToCString();
  return atoi(sourceIdText.get());
}

// Get the script from an ID.
static MaybeHandle<Script> MaybeGetScript(Isolate* isolate, int script_id) {
  CHECK(gRecordReplayScripts);
  auto iter = gRecordReplayScripts->find(script_id);
  if (iter == gRecordReplayScripts->end()) {
    return MaybeHandle<Script>();
  }

  Local<v8::Value> scriptValue = iter->second.Get((v8::Isolate*)isolate);
  Handle<Object> scriptObj = Utils::OpenHandle(*scriptValue);
  Handle<Script> script(Script::cast(*scriptObj), isolate);
  CHECK(script->id() == script_id);
  return script;
}

// Get the script from an ID.
Handle<Script> GetScript(Isolate* isolate, int script_id) {
  return MaybeGetScript(isolate, script_id).ToHandleChecked();
}

Handle<Object> RecordReplayGetSourceContents(Isolate* isolate, Handle<Object> params) {
  int script_id = GetSourceIdProperty(isolate, params);
  Handle<Script> script = GetScript(isolate, script_id);

  Script::PositionInfo info;
  Script::GetPositionInfo(script, 0, &info, Script::WITH_OFFSET);

  // Pad the start of the source with lines to adjust for its starting position.
  // Note that we don't pad the starting line with blank spaces so that columns
  // match up, in order to match the spidermonkey implementation.
  std::string padded_source;
  for (int i = 0; i < info.line; i++) {
    padded_source += "\n";
  }

  Handle<String> source(String::cast(script->source()), isolate);
  padded_source += source->ToCString().get();

  Handle<JSObject> obj = NewPlainObject(isolate);
  SetProperty(isolate, obj, "contents", padded_source.c_str());
  SetProperty(isolate, obj, "contentType", "text/javascript");
  return obj;
}

static void DecodeLocationProperty(Isolate* isolate, Handle<Object> params,
                                   const char* property, int* line, int* column) {
  Handle<Object> location = GetProperty(isolate, params, property);
  if (location->IsUndefined()) {
    return;
  }

  Handle<Object> lineProperty = GetProperty(isolate, location, "line");
  *line = lineProperty->Number();

  Handle<Object> columnProperty = GetProperty(isolate, location, "column");
  *column = columnProperty->Number();
}

static void ForEachInstrumentationOp(Isolate* isolate, Handle<Script> script,
                                     std::function<void(Handle<SharedFunctionInfo>,
                                                        int, bool)> aCallback) {
  // Based on Debug::GetPossibleBreakpoints.
  while (true) {
    HandleScope scope(isolate);
    std::vector<Handle<SharedFunctionInfo>> candidates;
    std::vector<IsCompiledScope> compiled_scopes;
    SharedFunctionInfo::ScriptIterator iterator(isolate, *script);
    for (SharedFunctionInfo info = iterator.Next(); !info.is_null();
         info = iterator.Next()) {
      if (!info.IsSubjectToDebugging()) continue;
      if (!info.is_compiled() && !info.allows_lazy_compilation()) continue;
      candidates.push_back(i::handle(info, isolate));
    }

    // Compile any uncompiled functions found in the script.
    bool was_compiled = false;
    for (const auto& candidate : candidates) {
      IsCompiledScope is_compiled_scope(candidate->is_compiled_scope(isolate));
      if (!is_compiled_scope.is_compiled()) {
        if (!Compiler::Compile(isolate, candidate, Compiler::CLEAR_EXCEPTION,
                               &is_compiled_scope)) {
          recordreplay::Print("Compiler::Compile failed, crashing.");
          CHECK(0);
        } else {
          was_compiled = true;
        }
      }
      DCHECK(is_compiled_scope.is_compiled());
      compiled_scopes.push_back(is_compiled_scope);
    }

    // If we did any compilation, restart and look for any new functions
    // that need to be compiled.
    if (was_compiled) continue;

    // Now we have a complete list of the functions in the script.
    // Build the final locations.
    for (const auto& candidate : candidates) {
      if (!candidate->HasBytecodeArray()) {
        continue;
      }
      Handle<BytecodeArray> bytecode(candidate->GetBytecodeArray(isolate), isolate);

      bool first = true;
      for (interpreter::BytecodeArrayIterator it(bytecode); !it.done();
           it.Advance()) {
        interpreter::Bytecode bytecode = it.current_bytecode();
        if (bytecode == interpreter::Bytecode::kRecordReplayInstrumentation) {
          int index = it.GetIndexOperand(0);
          aCallback(candidate, index, first);
          first = false;
        }
      }
    }
    return;
  }
}

// Information about breakpoints that have been sent to the record replay driver.
struct BreakpointInfo {
  std::string function_id_;
  int bytecode_offset_;
  BreakpointInfo(const std::string& function_id, int bytecode_offset)
    : function_id_(function_id), bytecode_offset_(bytecode_offset) {}
};
typedef std::unordered_map<std::string, BreakpointInfo> BreakpointInfoMap;
static BreakpointInfoMap* gBreakpoints;

static std::string BreakpointKey(int script_id, int line, int column) {
  std::ostringstream os;
  os << script_id << ":" << line << ":" << column;
  return os.str();
}

// Inverse of gBreakpoints mapping.
struct BreakpointPosition {
  int line_;
  int column_;
  BreakpointPosition(int line, int column)
    : line_(line), column_(column) {}
};
typedef std::unordered_map<std::string, BreakpointPosition> BreakpointPositionMap;
static BreakpointPositionMap* gBreakpointPositions;

static std::string BreakpointPositionKey(std::string function_id,
                                         int bytecode_offset) {
  std::ostringstream os;
  os << function_id << ":" << bytecode_offset;
  return os.str();
}

extern const char* InstrumentationSiteKind(int index);
extern int InstrumentationSiteSourcePosition(int index);
extern int InstrumentationSiteBytecodeOffset(int index);
extern std::string GetRecordReplayFunctionId(Handle<SharedFunctionInfo> shared);

static void GetInstrumentationSiteLocation(Handle<Script> script, int instrumentation_index,
                                           int* pline, int* pcolumn) {
  int source_position = InstrumentationSiteSourcePosition(instrumentation_index);
  Script::PositionInfo info;
  Script::GetPositionInfo(script, source_position, &info, Script::WITH_OFFSET);

  // Use 1-indexed lines instead of 0-indexed.
  *pline = info.line + 1;
  *pcolumn = info.column;
}

static void ForEachInstrumentationOpInRange(
  Isolate* isolate, Handle<Object> params,
  const std::function<void(Handle<Script> script, int bytecode_offset,
                           const std::string& function_id, int line, int column)> callback) {
  int script_id = GetSourceIdProperty(isolate, params);
  Handle<Script> script = GetScript(isolate, script_id);

  int beginLine = 1, beginColumn = 0;
  DecodeLocationProperty(isolate, params, "begin", &beginLine, &beginColumn);

  int endLine = INT32_MAX, endColumn = INT32_MAX;
  DecodeLocationProperty(isolate, params, "end", &endLine, &endColumn);

  ForEachInstrumentationOp(isolate, script, [&](Handle<SharedFunctionInfo> shared,
                                                int instrumentation_index, bool first) {
    if (strcmp(InstrumentationSiteKind(instrumentation_index), "breakpoint")) {
      return;
    }

    int line, column;
    GetInstrumentationSiteLocation(script, instrumentation_index, &line, &column);

    if (line < beginLine ||
        (line == beginLine && column < beginColumn) ||
        line > endLine ||
        (line == endLine && column > endColumn)) {
      return;
    }

    int bytecode_offset = InstrumentationSiteBytecodeOffset(instrumentation_index);

    std::string function_id = GetRecordReplayFunctionId(shared);
    callback(script, bytecode_offset, function_id, line, column);
  });
}

static void GenerateBreakpointInfo(Isolate* isolate, Handle<Script> script) {
  if (!gBreakpoints) {
    gBreakpoints = new BreakpointInfoMap();
  }
  if (!gBreakpointPositions) {
    gBreakpointPositions = new BreakpointPositionMap();
  }

  ForEachInstrumentationOp(isolate, script, [&](Handle<SharedFunctionInfo> shared,
                                                int instrumentation_index, bool first) {
    int line, column;
    GetInstrumentationSiteLocation(script, instrumentation_index, &line, &column);

    std::string function_id = GetRecordReplayFunctionId(shared);
    int bytecode_offset = InstrumentationSiteBytecodeOffset(instrumentation_index);

    std::string key = BreakpointKey(script->id(), line, column);
    BreakpointInfo value(function_id, bytecode_offset);
    gBreakpoints->insert(std::pair<std::string, BreakpointInfo>
                         (key, value));

    std::string positionKey = BreakpointPositionKey(function_id, bytecode_offset);
    BreakpointPosition position(line, column);
    gBreakpointPositions->insert(std::pair<std::string, BreakpointPosition>
                                 (positionKey, position));
  });
}

static Handle<Object> RecordReplayGetPossibleBreakpoints(Isolate* isolate,
                                                         Handle<Object> params) {
  std::vector<std::vector<int>> lineColumns;
  size_t numLines = 0;

  ForEachInstrumentationOpInRange(isolate, params,
     [&](Handle<Script> script, int bytecode_offset,
         const std::string& function_id, int line, int column) {
    while ((size_t)line >= lineColumns.size()) {
      lineColumns.emplace_back();
    }
    if (!lineColumns[line].size()) {
      numLines++;
    }
    lineColumns[line].push_back(column);
  });

  Handle<FixedArray> lineLocations = isolate->factory()->NewFixedArray(numLines);
  size_t lineLocationsIndex = 0;
  for (size_t line = 0; line < lineColumns.size(); line++) {
    const std::vector<int>& baseColumns = lineColumns[line];
    if (!baseColumns.size()) {
      continue;
    }

    Handle<FixedArray> columns = isolate->factory()->NewFixedArray(baseColumns.size());
    for (size_t i = 0; i < baseColumns.size(); i++) {
      columns->set(i, Smi::FromInt(baseColumns[i]));
    }
    Handle<JSArray> columnsArray = isolate->factory()->NewJSArrayWithElements(columns);

    Handle<JSObject> lineObj = NewPlainObject(isolate);
    SetProperty(isolate, lineObj, "line", line);
    SetProperty(isolate, lineObj, "columns", columnsArray);
    lineLocations->set(lineLocationsIndex++, *lineObj);
  }
  DCHECK(lineLocationsIndex == numLines);

  Handle<JSArray> lineLocationsArray =
    isolate->factory()->NewJSArrayWithElements(lineLocations);

  Handle<JSObject> rv = NewPlainObject(isolate);
  SetProperty(isolate, rv, "lineLocations", lineLocationsArray);
  return rv;
}

// Make sure that the isolate has a context by switching to the default
// context if necessary.
static void EnsureIsolateContext(Isolate* isolate, base::Optional<SaveAndSwitchContext>& ssc) {
  CHECK(!isolate->context().is_null());
}

extern void RecordReplayAddPossibleBreakpoint(int line, int column, const char* function, int offset);

void PossibleBreakpointsCallback(const char* source_id) {
  CHECK(IsMainThread());
  recordreplay::AutoDisallowEvents disallow;

  Isolate* isolate = Isolate::Current();

  base::Optional<SaveAndSwitchContext> ssc;
  EnsureIsolateContext(isolate, ssc);

  HandleScope scope(isolate);

  Handle<Script> script = GetScript(isolate, atoi(source_id));

  std::string currentFunctionId;

  ForEachInstrumentationOp(isolate, script, [&](Handle<SharedFunctionInfo> shared,
                                                int instrumentation_index, bool first) {
    if (first) {
      currentFunctionId = GetRecordReplayFunctionId(shared);
    }

    if (strcmp(InstrumentationSiteKind(instrumentation_index), "breakpoint")) {
      return;
    }

    int line, column;
    GetInstrumentationSiteLocation(script, instrumentation_index, &line, &column);

    int offset = InstrumentationSiteBytecodeOffset(instrumentation_index);
    RecordReplayAddPossibleBreakpoint(line, column, currentFunctionId.c_str(), offset);
  });
}

Handle<Object> RecordReplayConvertLocationToFunctionOffset(Isolate* isolate,
                                                           Handle<Object> params) {
  Handle<Object> location = GetProperty(isolate, params, "location");
  int sourceId = GetSourceIdProperty(isolate, location);
  int line = GetProperty(isolate, location, "line")->Number();
  int column = GetProperty(isolate, location, "column")->Number();

  std::string key = BreakpointKey(sourceId, line, column);
  if (!gBreakpoints) {
    Handle<Script> script = GetScript(isolate, sourceId);
    GenerateBreakpointInfo(isolate, script);
  }
  auto iter = gBreakpoints->find(key);
  if (iter == gBreakpoints->end()) {
    Handle<Script> script = GetScript(isolate, sourceId);
    GenerateBreakpointInfo(isolate, script);

    iter = gBreakpoints->find(key);
    if (iter == gBreakpoints->end()) {
      return NewPlainObject(isolate);
    }
  }

  Handle<JSObject> rv = NewPlainObject(isolate);
  SetProperty(isolate, rv, "functionId", iter->second.function_id_.c_str());
  SetProperty(isolate, rv, "offset", iter->second.bytecode_offset_);
  return rv;
}

static Handle<String> GetProtocolSourceId(Isolate* isolate, Handle<Script> script) {
  std::ostringstream os;
  os << script->id();
  return CStringToHandle(isolate, os.str().c_str());
}

extern void ParseRecordReplayFunctionId(const std::string& function_id,
                                        int* script_id, int* source_position);

static Handle<Object> RecordReplayConvertFunctionOffsetToLocation(Isolate* isolate,
                                                                  Handle<Object> params) {
  Handle<Object> function_id_raw = GetProperty(isolate, params, "functionId");

  std::unique_ptr<char[]> function_id_chars = String::cast(*function_id_raw).ToCString();
  std::string function_id(function_id_chars.get());
  int script_id;
  int function_source_position;
  ParseRecordReplayFunctionId(function_id,
                              &script_id, &function_source_position);

  Handle<Object> offset_raw = GetProperty(isolate, params, "offset");

  Handle<Script> script = GetScript(isolate, script_id);

  // The offset may or may not be present. If it isn't present then we parse the
  // function ID to get the source position, otherwise use the offset as the
  // instrumentation site to get the source position.
  int line, column;
  if (offset_raw->IsUndefined()) {
    Script::PositionInfo info;
    Script::GetPositionInfo(script, function_source_position, &info, Script::WITH_OFFSET);

    // Use 1-indexed lines instead of 0-indexed.
    line = info.line + 1;
    column = info.column;
  } else {
    int bytecode_offset = offset_raw->Number();

    std::string key = BreakpointPositionKey(function_id, bytecode_offset);
    if (!gBreakpointPositions) {
      GenerateBreakpointInfo(isolate, script);
    }
    auto iter = gBreakpointPositions->find(key);
    if (iter == gBreakpointPositions->end()) {
      GenerateBreakpointInfo(isolate, script);

      iter = gBreakpointPositions->find(key);
      if (iter == gBreakpointPositions->end()) {
        recordreplay::Diagnostic("Unknown offset %s %d for RecordReplayConvertFunctionOffsetToLocation, crashing.",
                                 function_id.c_str(), bytecode_offset);
        CHECK(0);
      }
    }

    line = iter->second.line_;
    column = iter->second.column_;
  }

  Handle<JSObject> location = NewPlainObject(isolate);
  SetProperty(isolate, location, "sourceId", GetProtocolSourceId(isolate, script));
  SetProperty(isolate, location, "line", line);
  SetProperty(isolate, location, "column", column);

  Handle<JSObject> rv = NewPlainObject(isolate);
  SetProperty(isolate, rv, "location", location);
  return rv;
}

bool RecordReplayIgnoreScript(Script script);

static Handle<Object> RecordReplayCountStackFrames(Isolate* isolate,
                                                   Handle<Object> params) {
  // This is handled in C++ instead of via a protocol JS handler for efficiency.
  // Counting the stack frames is a common operation when there are many
  // exception unwinds and so forth.
  size_t count = 0;
  for (JavaScriptFrameIterator it(isolate); !it.done(); it.Advance()) {
    JavaScriptFrame* frame = JavaScriptFrame::cast(it.frame());
    if (frame->type() != StackFrame::OPTIMIZED && frame->type() != StackFrame::INTERPRETED) {
      continue;
    }
    std::vector<FrameSummary> frames;
    frame->Summarize(&frames);

    // We don't strictly need to iterate the frames in reverse order, but it
    // helps when logging the stack contents for debugging.
    for (int i = (int)frames.size() - 1; i >= 0; i--) {
      const auto& summary = frames[i];
      CHECK(summary.IsJavaScript());
      const auto& js = summary.AsJavaScript();

      Handle<SharedFunctionInfo> shared(js.function()->shared(), isolate);

      // See GetStackLocation.
      if (!shared->StartPosition() && !shared->EndPosition()) {
        continue;
      }

      Handle<Script> script(Script::cast(shared->script()), isolate);
      if (script->id() && !RecordReplayIgnoreScript(*script)) {
        count++;
      }
    }
  }

  Handle<JSObject> rv = NewPlainObject(isolate);
  SetProperty(isolate, rv, "count", count);
  return rv;
}

static Handle<Object> RecordReplayGetFunctionsInRange(Isolate* isolate,
                                                      Handle<Object> params) {
  std::set<std::string> functions;
  ForEachInstrumentationOpInRange(isolate, params,
     [&](Handle<Script> script, int bytecode_offset,
         const std::string& function_id, int line, int column) {
    functions.insert(function_id);
  });

  Handle<FixedArray> functionsArray = isolate->factory()->NewFixedArray(functions.size());

  size_t index = 0;
  for (const std::string& function_id : functions) {
    Handle<String> str = CStringToHandle(isolate, function_id.c_str());
    functionsArray->set(index++, *str);
  }
  CHECK(index == functions.size());

  Handle<JSArray> functionsJSArray =
    isolate->factory()->NewJSArrayWithElements(functionsArray);

  Handle<JSObject> rv = NewPlainObject(isolate);
  SetProperty(isolate, rv, "functions", functionsJSArray);
  return rv;
}

extern int RecordReplayCurrentGeneratorIdRaw();

static Handle<Object> RecordReplayCurrentGeneratorId(Isolate* isolate, Handle<Object> params) {
  Handle<JSObject> rv = NewPlainObject(isolate);
  int id = RecordReplayCurrentGeneratorIdRaw();
  if (id) {
    SetProperty(isolate, rv, "id", id);
  }
  return rv;
}

static Handle<Object> RecordReplayGetStackFunctionIds(Isolate* isolate, Handle<Object> params) {
  std::vector<std::string> functions;
  for (JavaScriptFrameIterator it(isolate); !it.done(); it.Advance()) {
    JavaScriptFrame* frame = JavaScriptFrame::cast(it.frame());
    if (frame->type() != StackFrame::OPTIMIZED && frame->type() != StackFrame::INTERPRETED) {
      continue;
    }
    std::vector<FrameSummary> frames;
    frame->Summarize(&frames);

    for (int i = (int)frames.size() - 1; i >= 0; i--) {
      const auto& summary = frames[i];
      CHECK(summary.IsJavaScript());
      const auto& js = summary.AsJavaScript();

      Handle<SharedFunctionInfo> shared(js.function()->shared(), isolate);

      // See GetStackLocation.
      if (!shared->StartPosition() && !shared->EndPosition()) {
        continue;
      }

      Handle<Script> script(Script::cast(shared->script()), isolate);
      if (script->id() && !RecordReplayIgnoreScript(*script)) {
        functions.push_back(GetRecordReplayFunctionId(shared));
      }
    }
  }

  Handle<FixedArray> functionsArray = isolate->factory()->NewFixedArray(functions.size());

  size_t index = 0;
  for (const std::string& function_id : functions) {
    Handle<String> str = CStringToHandle(isolate, function_id.c_str());
    functionsArray->set(index++, *str);
  }
  CHECK(index == functions.size());

  Handle<JSArray> functionsJSArray =
    isolate->factory()->NewJSArrayWithElements(functionsArray);

  Handle<JSObject> rv = NewPlainObject(isolate);
  SetProperty(isolate, rv, "frameFunctions", functionsJSArray);
  return rv;
}

extern bool RecordReplayInstrumentNodeInternals();

bool RecordReplayIgnoreScriptByURL(const char* url) {
  // Always ignore V8 internal JS.
  if (!strcmp(url, "v8/externalize") ||
      !strcmp(url, "v8/gc")) {
    return true;
  }

  if (RecordReplayInstrumentNodeInternals()) {
    // When exposing node internals, we still ignore the record/replay specific
    // scripts, as these will have on stack frames when processing commands.
    if (strstr(url, "node:internal/recordreplay")) {
      return true;
    }

    // This causes problems with stack size mismatches where the main module
    // has been entered but the frame does not appear on stack. The underlying
    // cause is unknown.
    if (strstr(url, "node:internal/main/run_main_module")) {
      return true;
    }

    // Ignore node code that can run before the first checkpoint is created.
    if (!strcmp(url, "node:events")) {
      return true;
    }

    return false;
  }

  // Normally we ignore node internal scripts entirely.
  return !strncmp(url, "node:", 5);
}

static void RecordReplayRegisterScript(Handle<Script> script) {
  CHECK(IsMainThread());

  if (!gRecordReplayScripts) {
    gRecordReplayScripts = new ScriptIdMap();
  }
  auto iter = gRecordReplayScripts->find(script->id());
  if (iter != gRecordReplayScripts->end()) {
    // Ignore duplicate registers.
    return;
  }

  Isolate* isolate = Isolate::Current();

  (*gRecordReplayScripts)[script->id()] =
    Eternal<Value>((v8::Isolate*)isolate, v8::Utils::ToLocal(script));

  Handle<String> idStr = GetProtocolSourceId(isolate, script);
  std::unique_ptr<char[]> id = String::cast(*idStr).ToCString();

  if (script->type() == Script::TYPE_WASM) {
    return;
  }

  std::string url;
  if (!script->name().IsUndefined()) {
    std::unique_ptr<char[]> name = String::cast(script->name()).ToCString();
    if (RecordReplayIgnoreScriptByURL(name.get())) {
      return;
    }
    url = std::string("file://") + name.get();
  }

  RecordReplayOnNewSource(isolate, id.get(), "scriptSource", url.length() ? url.c_str() : nullptr);

  // If this is the first script we were notified about, look for other scripts
  // that were already added without a notification. It would be nice to figure
  // out how to get notified about the other scripts and remove this...
  static bool first = true;
  if (first) {
    first = false;
    std::vector<Handle<Script>> scriptHandles;
    {
      Script::Iterator iterator(isolate);
      for (Script script = iterator.Next(); !script.is_null();
           script = iterator.Next()) {
        if (script.HasValidSource()) {
          Handle<Script> handle(script, isolate);
          scriptHandles.push_back(handle);
        }
      }
    }
    for (Handle<Script> handle : scriptHandles) {
      RecordReplayRegisterScript(handle);
    }
  }
}

extern void RecordReplayOnConsoleMessage(size_t bookmark);

// Command callbacks which we handle directly.
struct InternalCommandCallback {
  const char* mCommand;
  Handle<Object> (*mCallback)(Isolate* isolate, Handle<Object> params);
};
static InternalCommandCallback gInternalCommandCallbacks[] = {
  { "Debugger.getSourceContents", RecordReplayGetSourceContents },
  { "Debugger.getPossibleBreakpoints", RecordReplayGetPossibleBreakpoints },
  { "Target.convertLocationToFunctionOffset", RecordReplayConvertLocationToFunctionOffset },
  { "Target.convertFunctionOffsetToLocation", RecordReplayConvertFunctionOffsetToLocation },
  { "Target.countStackFrames", RecordReplayCountStackFrames },
  { "Target.getFunctionsInRange", RecordReplayGetFunctionsInRange },
  { "Target.currentGeneratorId", RecordReplayCurrentGeneratorId },
  { "Target.getStackFunctionIDs", RecordReplayGetStackFunctionIds },
};

// Function to invoke on command callbacks which we don't have a C++ implementation for.
static Eternal<Value>* gCommandCallback;

char* CommandCallback(const char* command, const char* params) {
  CHECK(IsMainThread());
  recordreplay::AutoDisallowEvents disallow;

  Isolate* isolate = Isolate::Current();
  base::Optional<SaveAndSwitchContext> ssc;
  EnsureIsolateContext(isolate, ssc);

  HandleScope scope(isolate);

  Handle<Object> undefined = isolate->factory()->undefined_value();
  Handle<String> paramsStr = CStringToHandle(isolate, params);

  MaybeHandle<Object> maybeParams = JsonParser<uint8_t>::Parse(isolate, paramsStr, undefined);
  if (maybeParams.is_null()) {
    recordreplay::Diagnostic("Error: CommandCallbackWrapper Parse %s failed", params);
    CHECK(0);
  }
  Handle<Object> paramsObj = maybeParams.ToHandleChecked();

  MaybeHandle<Object> rv;
  for (const InternalCommandCallback& cb : gInternalCommandCallbacks) {
    if (!strcmp(cb.mCommand, command)) {
      rv = cb.mCallback(isolate, paramsObj);
      CHECK(!rv.is_null());
    }
  }
  if (rv.is_null()) {
    if (!gCommandCallback) {
      // Handle commands sent at the start of the recording.
      return strdup("{ \"error\": \"Command callback not installed\" }");
    }
    Local<v8::Value> callbackValue = gCommandCallback->Get((v8::Isolate*)isolate);
    Handle<Object> callback = Utils::OpenHandle(*callbackValue);

    Handle<Object> callArgs[2];
    callArgs[0] = CStringToHandle(isolate, command);
    callArgs[1] = paramsObj;
    rv = Execution::Call(isolate, callback, undefined, 2, callArgs);
    CHECK(!rv.is_null());
  }

  Handle<Object> rvStr =
    JsonStringify(isolate, rv.ToHandleChecked(), undefined, undefined).ToHandleChecked();
  std::unique_ptr<char[]> rvCStr = String::cast(*rvStr).ToCString();
  return strdup(rvCStr.get());
}

static Eternal<Value>* gClearPauseDataCallback;

void ClearPauseDataCallback() {
  CHECK(IsMainThread());
  recordreplay::AutoDisallowEvents disallow;

  if (!gClearPauseDataCallback) {
    return;
  }

  Isolate* isolate = Isolate::Current();
  base::Optional<SaveAndSwitchContext> ssc;
  EnsureIsolateContext(isolate, ssc);

  HandleScope scope(isolate);

  Local<v8::Value> callbackValue = gClearPauseDataCallback->Get((v8::Isolate*)isolate);
  Handle<Object> callback = Utils::OpenHandle(*callbackValue);

  Handle<Object> undefined = isolate->factory()->undefined_value();
  MaybeHandle<Object> rv = Execution::Call(isolate, callback, undefined, 0, nullptr);
  CHECK(!rv.is_null());
}

typedef std::unordered_map<int, bool> ScriptIdIgnoreMap;
static ScriptIdIgnoreMap* gShouldIgnoreScripts;

static bool RecordReplayIgnoreScriptRaw(Script script) {
  if (script.type() == Script::TYPE_WASM) {
    return true;
  }

  if (script.name().IsUndefined()) {
    return false;
  }

  std::unique_ptr<char[]> name = String::cast(script.name()).ToCString();
  return RecordReplayIgnoreScriptByURL(name.get());
}

bool RecordReplayIgnoreScript(Script script) {
  if (!IsMainThread()) {
    return true;
  }

  if (!gShouldIgnoreScripts) {
    gShouldIgnoreScripts = new ScriptIdIgnoreMap();
  }
  auto iter = gShouldIgnoreScripts->find(script.id());
  if (iter != gShouldIgnoreScripts->end()) {
    return iter->second;
  }

  bool rv = RecordReplayIgnoreScriptRaw(script);
  (*gShouldIgnoreScripts)[script.id()] = rv;
  return rv;
}

static bool RecordReplayIgnoreScriptById(Isolate* isolate, int script_id) {
  Handle<Script> script = GetScript(isolate, script_id);
  return RecordReplayIgnoreScript(*script);
}

static std::string StringPrintf(const char* format, ...) {
  char buf[4096];
  buf[sizeof(buf) - 1] = 0;
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf) - 1, format, ap);
  va_end(ap);
  return std::string(buf);
}

// When assertions are used we assign an ID to each object that is ever
// encountered in one, so that we can determine whether consistent objects
// are used when replaying.
struct ContextObjectIdMap {
  v8::Global<v8::Context> context_;
  v8::Global<v8::debug::WeakMap> object_ids_;
};
typedef std::vector<ContextObjectIdMap> ContextObjectIdMapVector;
static ContextObjectIdMapVector* gRecordReplayObjectIds;

static Local<v8::debug::WeakMap> GetObjectIdMapForContext(v8::Isolate* isolate, Local<v8::Context> cx) {
  if (!gRecordReplayObjectIds) {
    gRecordReplayObjectIds = new ContextObjectIdMapVector();
  }

  for (const auto& entry : *gRecordReplayObjectIds) {
    if (entry.context_ == cx) {
      return entry.object_ids_.Get(isolate);
    }
  }

  ContextObjectIdMap new_entry;
  new_entry.context_.Reset(isolate, cx);
  new_entry.object_ids_.Reset(isolate, v8::debug::WeakMap::New(isolate));
  gRecordReplayObjectIds->push_back(std::move(new_entry));
  return gRecordReplayObjectIds->back().object_ids_.Get(isolate);
}

static int gNextObjectId = 1;

int RecordReplayObjectId(Handle<Object> internal_object) {
  CHECK(IsMainThread());
  v8::Isolate* isolate = v8::Isolate::GetCurrent();

  v8::Local<v8::Value> object = v8::Utils::ToLocal(internal_object);

  Local<v8::Context> cx = isolate->GetCurrentContext();
  Local<v8::debug::WeakMap> object_ids = GetObjectIdMapForContext(isolate, cx);

  v8::Local<v8::Value> idValue;
  if (object_ids->Get(cx, object).ToLocal(&idValue) && idValue->IsInt32()) {
    return idValue.As<v8::Int32>()->Value();
  }

  int id = gNextObjectId++;
  Local<Value> id_value = v8::Integer::New(isolate, id);
  object_ids->Set(cx, object, id_value).ToLocalChecked();
  return id;
}

inline int HashBytes(const void* aPtr, size_t aSize) {
  int hash = 0;
  uint8_t* ptr = (uint8_t*)aPtr;
  for (size_t i = 0; i < aSize; i++) {
    hash = (((hash << 5) - hash) + ptr[i]) | 0;
  }
  return hash;
}

// Get a string describing a value which can be used in assertions.
// Only basic information about the value is obtained, to keep things fast.
std::string RecordReplayBasicValueContents(Handle<Object> value) {
  if (value->IsNumber()) {
    double num = value->Number();
    if (std::isnan(num)) {
      return "NaN";
    }
    return StringPrintf("Number %d %llu", (int)num, *(uint64_t*)&num);
  }

  if (value->IsBoolean()) {
    return StringPrintf("Boolean %d", value->IsTrue());
  }

  if (value->IsUndefined()) {
    return "Undefined";
  }

  if (value->IsNull()) {
    return "Null";
  }

  if (value->IsString()) {
    String str = String::cast(*value);
    if (str.length() <= 200) {
      std::unique_ptr<char[]> name = str.ToCString();
      return StringPrintf("String %s", name.get());
    }
    return StringPrintf("LongString %d", str.length());
  }

  if (value->IsJSObject()) {
    int object_id = RecordReplayObjectId(value);

    InstanceType type = JSObject::cast(*value).map().instance_type();
    const char* typeStr;
    switch (type) {
#define STRINGIFY_TYPE(TYPE) case TYPE: typeStr = #TYPE; break;
    INSTANCE_TYPE_LIST(STRINGIFY_TYPE)
#undef STRINGIFY_TYPE
    default:
      typeStr = "<unknown>";
    }
    if (!strcmp(typeStr, "JS_DATE_TYPE")) {
      JSDate date = JSDate::cast(*value);
      double time = date.value().Number();
      return StringPrintf("Date %d %.2f", object_id, time);
    }
    if (!strcmp(typeStr, "JS_TYPED_ARRAY_TYPE")) {
      v8::Local<v8::Value> obj = v8::Utils::ToLocal(value);
      v8::Local<v8::TypedArray> tarr = obj.As<v8::TypedArray>();
      char buf[50];
      size_t written = tarr->CopyContents(buf, sizeof(buf));
      int hash = HashBytes(buf, written);
      return StringPrintf("TypedArray %d %lu %d", object_id, tarr->ByteLength(), hash);
    }
    return StringPrintf("Object %d %s", object_id, typeStr);
  }

  if (value->IsJSProxy()) {
    return "Proxy";
  }

  return "Unknown";
}

void Debug::SetTemporaryObjectTrackingDisabled(bool disabled) {
  if (temporary_objects_) {
    temporary_objects_->disabled = disabled;
  }
}

bool Debug::GetTemporaryObjectTrackingDisabled() const {
  if (temporary_objects_) {
    return temporary_objects_->disabled;
  }
  return false;
}

}  // namespace internal

namespace i = internal;

void FunctionCallbackIsRecordingOrReplaying(const FunctionCallbackInfo<Value>& callArgs) {
  Local<Boolean> rv = Boolean::New(callArgs.GetIsolate(), recordreplay::IsRecordingOrReplaying());
  callArgs.GetReturnValue().Set(rv);
}

void FunctionCallbackRecordReplayOnConsoleAPI(const FunctionCallbackInfo<Value>& callArgs) {
  CHECK(recordreplay::IsRecordingOrReplaying());
  if (IsMainThread()) {
    i::RecordReplayOnConsoleMessage(0);
  }
}

void FunctionCallbackRecordReplaySetCommandCallback(const FunctionCallbackInfo<Value>& callArgs) {
  CHECK(recordreplay::IsRecordingOrReplaying());
  CHECK(IsMainThread());
  CHECK(!i::gCommandCallback);

  Isolate* v8isolate = callArgs.GetIsolate();
  i::gCommandCallback = new Eternal<Value>(v8isolate, callArgs[0]);
}

void FunctionCallbackRecordReplaySetClearPauseDataCallback(const FunctionCallbackInfo<Value>& callArgs) {
  CHECK(recordreplay::IsRecordingOrReplaying());
  CHECK(IsMainThread());
  CHECK(!i::gClearPauseDataCallback);

  Isolate* v8isolate = callArgs.GetIsolate();
  i::gClearPauseDataCallback = new Eternal<Value>(v8isolate, callArgs[0]);
}

void FunctionCallbackRecordReplayIgnoreScript(const FunctionCallbackInfo<Value>& callArgs) {
  CHECK(recordreplay::IsRecordingOrReplaying());
  CHECK(IsMainThread());

  Isolate* isolate = callArgs.GetIsolate();

  i::Handle<i::Object> base = Utils::OpenHandle(*callArgs[0]);
  std::unique_ptr<char[]> name = i::String::cast(*base).ToCString();
  int script_id = atoi(name.get());

  bool ignore = i::RecordReplayIgnoreScriptById((i::Isolate*)isolate, script_id);

  Local<Boolean> rv = Boolean::New(isolate, ignore);
  callArgs.GetReturnValue().Set(rv);
}

void FunctionCallbackRecordReplayAssert(const FunctionCallbackInfo<Value>& callArgs) {
  if (!recordreplay::IsRecordingOrReplaying()) {
    return;
  }

  i::Handle<i::Object> value = Utils::OpenHandle(*callArgs[0]);

  // This is used when a script explicitly asserts the contents of a value, so we can do
  // more thorough checking.
  if (value->IsString()) {
    std::unique_ptr<char[]> contents = i::String::cast(*value).ToCString();
    size_t len = strlen(contents.get());
    if (len < 2000) {
      recordreplay::Assert("AssertValue StringContents %s", contents.get());
    } else {
      recordreplay::AssertBytes("AssertValue StringBytes", contents.get(), len);
    }
  } else {
    std::string contents = i::RecordReplayBasicValueContents(value);
    recordreplay::Assert("AssertValue %s", contents.c_str());
  }
}

static i::Handle<i::JSMessageObject>* gCurrentErrorEvent;

extern "C" void V8RecordReplayOnErrorEvent(v8::Local<v8::Message> message) {
  if (!recordreplay::IsRecordingOrReplaying()) {
    return;
  }

  auto self = Utils::OpenHandle(*message);

  gCurrentErrorEvent = &self;
  i::RecordReplayOnConsoleMessage(self->record_replay_bookmark());
  gCurrentErrorEvent = nullptr;
}

void FunctionCallbackRecordReplayGetCurrentError(const FunctionCallbackInfo<Value>& args) {
  if (!gCurrentErrorEvent) {
    return;
  }

  i::Isolate* isolate = (i::Isolate*) args.GetIsolate();
  i::Handle<i::Object> rv = i::NewPlainObject(isolate);

  auto msg = *gCurrentErrorEvent;
  i::Handle<i::String> message = i::MessageHandler::GetMessage(isolate, msg);
  i::Handle<i::Script> script(msg->script(), isolate);

  std::string url;
  if (!script->name().IsUndefined()) {
    std::unique_ptr<char[]> name = i::String::cast(script->name()).ToCString();
    url = std::string("file://") + name.get();
  }

  i::JSMessageObject::EnsureSourcePositionsAvailable(isolate, msg);

  i::SetProperty(isolate, rv, "message", message);
  i::SetProperty(isolate, rv, "filename", url.c_str());
  i::SetProperty(isolate, rv, "line", msg->GetLineNumber());
  i::SetProperty(isolate, rv, "column", msg->GetColumnNumber());
  i::SetProperty(isolate, rv, "scriptId", GetProtocolSourceId(isolate, script));

  args.GetReturnValue().Set(Utils::ToLocal(rv));
}

extern std::string RecordReplayGetRecordingId();

void FunctionCallbackRecordReplayGetRecordingId(const FunctionCallbackInfo<Value>& args) {
  if (!recordreplay::IsRecordingOrReplaying()) {
    return;
  }

  i::Isolate* isolate = (i::Isolate*) args.GetIsolate();

  std::string recordingId = RecordReplayGetRecordingId();
  i::Handle<i::String> rv = CStringToHandle(isolate, recordingId.c_str());
  args.GetReturnValue().Set(Utils::ToLocal(rv));
}

extern std::string RecordReplayGetCurrentExecutionPoint();
extern void RecordReplayNewCheckpointFlushed();

// When CurrentExecutionPoint has been used there isn't a guarantee that the point
// can be visited until we call NewCheckpointFlushed(). Ensure there is a timer
// running that makes sure this will be called soon if this API has been used.
static bool gHasNewCheckpointFlushedTask = false;

// How long to wait before ensuring the recording is flushed after getting the
// current execution point.
static double NewCheckpointFlushedDelaySeconds = 5.0;

void FunctionCallbackRecordReplayCurrentExecutionPoint(const FunctionCallbackInfo<Value>& args) {
  if (!recordreplay::IsRecordingOrReplaying() || !IsMainThread()) {
    return;
  }

  i::Isolate* isolate = (i::Isolate*) args.GetIsolate();

  std::string point = RecordReplayGetCurrentExecutionPoint();
  i::Handle<i::String> rv = CStringToHandle(isolate, point.c_str());
  args.GetReturnValue().Set(Utils::ToLocal(rv));

  if (!gHasNewCheckpointFlushedTask) {
    gHasNewCheckpointFlushedTask = true;

    auto task_runner = i::V8::GetCurrentPlatform()->GetForegroundTaskRunner(args.GetIsolate());
    auto task = i::MakeCancelableTask(isolate, []() {
      RecordReplayNewCheckpointFlushed();
      gHasNewCheckpointFlushedTask = false;
    });
    task_runner->PostDelayedTask(std::move(task), NewCheckpointFlushedDelaySeconds);
  }
}

extern size_t RecordReplayElapsedTimeMs();

void FunctionCallbackRecordReplayElapsedTimeMs(const FunctionCallbackInfo<Value>& args) {
  if (!recordreplay::IsRecordingOrReplaying()) {
    return;
  }

  Isolate* isolate = args.GetIsolate();

  size_t elapsed = RecordReplayElapsedTimeMs();
  Local<Value> elapsed_value = v8::Integer::New(isolate, elapsed);
  args.GetReturnValue().Set(elapsed_value);
}

}  // namespace v8
