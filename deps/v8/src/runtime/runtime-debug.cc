// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "src/common/globals.h"
#include "src/debug/debug-coverage.h"
#include "src/debug/debug-scopes.h"
#include "src/debug/debug.h"
#include "src/debug/liveedit.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/interpreter/bytecodes.h"
#include "src/interpreter/interpreter.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-collection-inl.h"
#include "src/objects/js-generator-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/js-weak-refs-inl.h"
#include "src/runtime/runtime-utils.h"
#include "src/runtime/runtime.h"
#include "src/snapshot/embedded/embedded-data.h"
#include "src/snapshot/snapshot.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/debug/debug-wasm-objects.h"
#include "src/wasm/wasm-objects-inl.h"
#endif  // V8_ENABLE_WEBASSEMBLY

#if !V8_OS_WIN
#include <sys/time.h>
#include <unistd.h>
#endif

#include "src/api/api-inl.h"
#include "src/base/replayio.h"

#include "src/builtins/builtins-iterator-inl.h"

namespace v8 {
namespace internal {

RUNTIME_FUNCTION(Runtime_IterableForEach) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<Object> iterable = args.at(0);
  Handle<JSReceiver> callback = args.at<JSReceiver>(1);

  auto int_visitor = [&](int val) -> bool {
    HandleScope loop_scope(isolate);
    DirectHandle<Object> argv[] = {isolate->factory()->NewNumberFromInt(val)};
    return !Execution::Call(isolate, callback,
                            isolate->factory()->undefined_value(),
                            base::VectorOf(argv))
                .is_null();
  };

  auto double_visitor = [&](double val) -> bool {
    HandleScope loop_scope(isolate);
    DirectHandle<Object> argv[] = {isolate->factory()->NewNumber(val)};
    return !Execution::Call(isolate, callback,
                            isolate->factory()->undefined_value(),
                            base::VectorOf(argv))
                .is_null();
  };

  auto generic_visitor = [&](DirectHandle<Object> val) -> bool {
    HandleScope loop_scope(isolate);
    DirectHandle<Object> argv[] = {val};
    return !Execution::Call(isolate, callback,
                            isolate->factory()->undefined_value(),
                            base::VectorOf(argv))
                .is_null();
  };

  if (IterableForEach<true>(isolate, iterable, int_visitor, double_visitor,
                            generic_visitor)
          .is_null()) {
    return ReadOnlyRoots(isolate).exception();
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION_RETURN_PAIR(Runtime_DebugBreakOnBytecode) {
  using interpreter::Bytecode;
  using interpreter::Bytecodes;
  using interpreter::OperandScale;

  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<Object> value = args.at(0);
  HandleScope scope(isolate);

  // Return value can be changed by debugger. Last set value will be used as
  // return value.
  ReturnValueScope result_scope(isolate->debug());
  isolate->debug()->set_return_value(*value);

  // Get the top-most JavaScript frame.
  JavaScriptStackFrameIterator it(isolate);
  if (isolate->debug_execution_mode() == DebugInfo::kBreakpoints) {
    isolate->debug()->Break(it.frame(),
                            direct_handle(it.frame()->function(), isolate));
  }

  // If the user requested to restart a frame, there is no need
  // to get the return value or check the bytecode for side-effects.
  if (isolate->debug()->IsRestartFrameScheduled()) {
    Tagged<Object> exception = isolate->TerminateExecution();
    return MakePair(exception,
                    Smi::FromInt(static_cast<uint8_t>(Bytecode::kIllegal)));
  }

  // Return the handler from the original bytecode array.
  DCHECK(it.frame()->is_interpreted());
  InterpretedFrame* interpreted_frame =
      reinterpret_cast<InterpretedFrame*>(it.frame());

  bool side_effect_check_failed = false;
  if (isolate->debug_execution_mode() == DebugInfo::kSideEffects) {
    side_effect_check_failed =
        !isolate->debug()->PerformSideEffectCheckAtBytecode(interpreted_frame);
  }

  // Make sure to only access these objects after the side effect check, as the
  // check can allocate on failure.
  Tagged<SharedFunctionInfo> shared = interpreted_frame->function()->shared();
  Tagged<BytecodeArray> bytecode_array = shared->GetBytecodeArray(isolate);
  int bytecode_offset = interpreted_frame->GetBytecodeOffset();
  Bytecode bytecode = Bytecodes::FromByte(bytecode_array->get(bytecode_offset));

  if (Bytecodes::Returns(bytecode)) {
    // If we are returning (or suspending), reset the bytecode array on the
    // interpreted stack frame to the non-debug variant so that the interpreter
    // entry trampoline sees the return/suspend bytecode rather than the
    // DebugBreak.
    interpreted_frame->PatchBytecodeArray(bytecode_array);
  }

  // We do not have to deal with operand scale here. If the bytecode at the
  // break is prefixed by operand scaling, we would have patched over the
  // scaling prefix. We now simply dispatch to the handler for the prefix.
  // We need to deserialize now to ensure we don't hit the debug break again
  // after deserializing.
  OperandScale operand_scale = OperandScale::kSingle;
  isolate->interpreter()->GetBytecodeHandler(bytecode, operand_scale);

  if (side_effect_check_failed) {
    return MakePair(ReadOnlyRoots(isolate).exception(),
                    Smi::FromInt(static_cast<uint8_t>(bytecode)));
  }
  Tagged<Object> interrupt_object = isolate->stack_guard()->HandleInterrupts();
  if (IsExceptionHole(interrupt_object)) {
    return MakePair(interrupt_object,
                    Smi::FromInt(static_cast<uint8_t>(bytecode)));
  }
  return MakePair(isolate->debug()->return_value(),
                  Smi::FromInt(static_cast<uint8_t>(bytecode)));
}

RUNTIME_FUNCTION(Runtime_DebugBreakAtEntry) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSFunction> function = args.at<JSFunction>(0);

  DCHECK(function->shared()->BreakAtEntry(isolate));

  // Get the top-most JavaScript frame. This is the debug target function.
  JavaScriptStackFrameIterator it(isolate);
  DCHECK_EQ(*function, it.frame()->function());
  // Check whether the next JS frame is closer than the last API entry.
  // if yes, then the call to the debug target came from JavaScript. Otherwise,
  // the call to the debug target came from API. Do not break for the latter.
  it.Advance();
  if (!it.done() &&
      it.frame()->fp() < isolate->thread_local_top()->last_api_entry_) {
    isolate->debug()->Break(it.frame(), function);
  }

  return function->shared()->GetCode(isolate);
}

RUNTIME_FUNCTION(Runtime_HandleDebuggerStatement) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  if (isolate->debug()->break_points_active()) {
    isolate->debug()->HandleDebugBreak(
        kIgnoreIfTopFrameBlackboxed,
        v8::debug::BreakReasons({v8::debug::BreakReason::kDebuggerStatement}));
    if (isolate->debug()->IsRestartFrameScheduled()) {
      return isolate->TerminateExecution();
    }
  }
  return isolate->stack_guard()->HandleInterrupts();
}

RUNTIME_FUNCTION(Runtime_ScheduleBreak) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  isolate->RequestInterrupt(
      [](v8::Isolate* isolate, void*) {
        v8::debug::BreakRightNow(
            isolate,
            v8::debug::BreakReasons({v8::debug::BreakReason::kScheduled}));
      },
      nullptr);
  return ReadOnlyRoots(isolate).undefined_value();
}

extern "C" void V8RecordReplayOnDebuggerStatement();

namespace {

template <class IteratorType>
static DirectHandle<ArrayList> AddIteratorInternalProperties(
    Isolate* isolate, DirectHandle<ArrayList> result,
    DirectHandle<IteratorType> iterator) {
  const char* kind = nullptr;
  switch (iterator->map()->instance_type()) {
    case JS_MAP_KEY_ITERATOR_TYPE:
      kind = "keys";
      break;
    case JS_MAP_KEY_VALUE_ITERATOR_TYPE:
    case JS_SET_KEY_VALUE_ITERATOR_TYPE:
      kind = "entries";
      break;
    case JS_MAP_VALUE_ITERATOR_TYPE:
    case JS_SET_VALUE_ITERATOR_TYPE:
      kind = "values";
      break;
    default:
      UNREACHABLE();
  }

  result = ArrayList::Add(
      isolate, result,
      isolate->factory()->NewStringFromAsciiChecked("[[IteratorHasMore]]"),
      isolate->factory()->ToBoolean(iterator->HasMore()));
  result = ArrayList::Add(
      isolate, result,
      isolate->factory()->NewStringFromAsciiChecked("[[IteratorIndex]]"),
      direct_handle(iterator->index(), isolate));
  result = ArrayList::Add(
      isolate, result,
      isolate->factory()->NewStringFromAsciiChecked("[[IteratorKind]]"),
      isolate->factory()->NewStringFromAsciiChecked(kind));
  return result;
}

}  // namespace

MaybeHandle<JSArray> Runtime::GetInternalProperties(
    Isolate* isolate, DirectHandle<Object> object) {
  DirectHandle<ArrayList> result = ArrayList::New(isolate, 8 * 2);
  if (IsJSObject(*object)) {
    PrototypeIterator iter(isolate, Cast<JSObject>(object), kStartAtReceiver);
    if (iter.HasAccess()) {
      iter.Advance();
      DirectHandle<Object> prototype = PrototypeIterator::GetCurrent(iter);
      if (!iter.IsAtEnd() && iter.HasAccess() && IsJSGlobalProxy(*object)) {
        // Skip JSGlobalObject as the [[Prototype]].
        DCHECK(IsJSGlobalObject(*prototype));
        iter.Advance();
        prototype = PrototypeIterator::GetCurrent(iter);
      }
      if (!IsNull(*prototype)) {
        result = ArrayList::Add(
            isolate, result,
            isolate->factory()->NewStringFromStaticChars("[[Prototype]]"),
            prototype);
      }
    }
  }
  if (IsJSBoundFunction(*object)) {
    auto function = Cast<JSBoundFunction>(object);

    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[TargetFunction]]"),
        direct_handle(function->bound_target_function(), isolate));
    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[BoundThis]]"),
        direct_handle(function->bound_this(), isolate));
    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[BoundArgs]]"),
        isolate->factory()->NewJSArrayWithElements(
            isolate->factory()->CopyFixedArray(
                handle(function->bound_arguments(), isolate))));
  } else if (IsJSMapIterator(*object)) {
    DirectHandle<JSMapIterator> iterator = Cast<JSMapIterator>(object);
    result = AddIteratorInternalProperties(isolate, result, iterator);
  } else if (IsJSSetIterator(*object)) {
    DirectHandle<JSSetIterator> iterator = Cast<JSSetIterator>(object);
    result = AddIteratorInternalProperties(isolate, result, iterator);
  } else if (IsJSGeneratorObject(*object)) {
    auto generator = Cast<JSGeneratorObject>(object);

    const char* status = "suspended";
    if (generator->is_closed()) {
      status = "closed";
    } else if (generator->is_executing()) {
      status = "running";
    } else {
      DCHECK(generator->is_suspended());
    }

    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[GeneratorState]]"),
        isolate->factory()->NewStringFromAsciiChecked(status));
    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[GeneratorFunction]]"),
        direct_handle(generator->function(), isolate));
    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[GeneratorReceiver]]"),
        direct_handle(generator->receiver(), isolate));
  } else if (IsJSPromise(*object)) {
    auto promise = Cast<JSPromise>(object);

    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[PromiseState]]"),
        isolate->factory()->NewStringFromAsciiChecked(
            JSPromise::Status(promise->status())));
    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[PromiseResult]]"),
        promise->status() == Promise::kPending
            ? isolate->factory()->undefined_value()
            : direct_handle(promise->result(), isolate));
  } else if (IsJSProxy(*object)) {
    auto js_proxy = Cast<JSProxy>(object);

    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[Handler]]"),
        direct_handle(js_proxy->handler(), isolate));
    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[Target]]"),
        direct_handle(js_proxy->target(), isolate));
    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[IsRevoked]]"),
        isolate->factory()->ToBoolean(js_proxy->IsRevoked()));
  } else if (IsJSPrimitiveWrapper(*object)) {
    auto js_value = Cast<JSPrimitiveWrapper>(object);

    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[PrimitiveValue]]"),
        direct_handle(js_value->value(), isolate));
  } else if (IsJSWeakRef(*object)) {
    auto js_weak_ref = Cast<JSWeakRef>(object);

    result = ArrayList::Add(
        isolate, result,
        isolate->factory()->NewStringFromAsciiChecked("[[WeakRefTarget]]"),
        direct_handle(js_weak_ref->target(), isolate));
  } else if (IsJSArrayBuffer(*object)) {
    DirectHandle<JSArrayBuffer> js_array_buffer = Cast<JSArrayBuffer>(object);
    if (js_array_buffer->was_detached()) {
      // Mark a detached JSArrayBuffer and such and don't even try to
      // create views for it, since the TypedArray constructors will
      // throw a TypeError when the underlying buffer is detached.
      result = ArrayList::Add(
          isolate, result,
          isolate->factory()->NewStringFromAsciiChecked("[[IsDetached]]"),
          isolate->factory()->true_value());
    } else {
      const size_t byte_length = js_array_buffer->byte_length();
      static_assert(JSTypedArray::kMaxByteLength ==
                    JSArrayBuffer::kMaxByteLength);
      using DataView = std::tuple<const char*, ExternalArrayType, size_t>;
      for (auto [name, type, elem_size] :
           {DataView{"[[Int8Array]]", kExternalInt8Array, 1},
            DataView{"[[Uint8Array]]", kExternalUint8Array, 1},
            DataView{"[[Int16Array]]", kExternalInt16Array, 2},
            DataView{"[[Int32Array]]", kExternalInt32Array, 4}}) {
        if ((byte_length % elem_size) != 0) continue;
        size_t length = byte_length / elem_size;
        result =
            ArrayList::Add(isolate, result,
                           isolate->factory()->NewStringFromAsciiChecked(name),
                           isolate->factory()->NewJSTypedArray(
                               type, js_array_buffer, 0, length));
      }
      result =
          ArrayList::Add(isolate, result,
                         isolate->factory()->NewStringFromAsciiChecked(
                             "[[ArrayBufferByteLength]]"),
                         isolate->factory()->NewNumberFromSize(byte_length));

      auto backing_store = js_array_buffer->GetBackingStore();
      DirectHandle<Object> array_buffer_data;
      if (backing_store) {
        array_buffer_data =
            isolate->factory()->NewNumberFromUint(backing_store->id());
      } else {
        array_buffer_data = isolate->factory()->null_value();
      }
      result = ArrayList::Add(
          isolate, result,
          isolate->factory()->NewStringFromAsciiChecked("[[ArrayBufferData]]"),
          array_buffer_data);

      DirectHandle<Symbol> memory_symbol =
          isolate->factory()->array_buffer_wasm_memory_symbol();
      DirectHandle<Object> memory_object =
          JSReceiver::GetDataProperty(isolate, js_array_buffer, memory_symbol);
      if (!IsUndefined(*memory_object)) {
        result = ArrayList::Add(isolate, result,
                                isolate->factory()->NewStringFromAsciiChecked(
                                    "[[WebAssemblyMemory]]"),
                                memory_object);
      }
    }
#if V8_ENABLE_WEBASSEMBLY
  } else if (IsWasmInstanceObject(*object)) {
    result = AddWasmInstanceObjectInternalProperties(
        isolate, result, Cast<WasmInstanceObject>(object));
  } else if (IsWasmModuleObject(*object)) {
    result = AddWasmModuleObjectInternalProperties(
        isolate, result, Cast<WasmModuleObject>(object));
  } else if (IsWasmTableObject(*object)) {
    result = AddWasmTableObjectInternalProperties(
        isolate, result, Cast<WasmTableObject>(object));
#endif  // V8_ENABLE_WEBASSEMBLY
  }
  return isolate->factory()->NewJSArrayWithElements(
      ArrayList::ToFixedArray(isolate, result), PACKED_ELEMENTS);
}

RUNTIME_FUNCTION(Runtime_GetGeneratorScopeCount) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());

  if (!IsJSGeneratorObject(args[0])) return Smi::zero();

  // Check arguments.
  Handle<JSGeneratorObject> gen = args.at<JSGeneratorObject>(0);

  // Only inspect suspended generator scopes.
  if (!gen->is_suspended()) {
    return Smi::zero();
  }

  // Count the visible scopes.
  int n = 0;
  for (ScopeIterator it(isolate, gen); !it.Done(); it.Next()) {
    n++;
  }

  return Smi::FromInt(n);
}

RUNTIME_FUNCTION(Runtime_GetGeneratorScopeDetails) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  if (!IsJSGeneratorObject(args[0])) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  // Check arguments.
  Handle<JSGeneratorObject> gen = args.at<JSGeneratorObject>(0);
  int index = NumberToInt32(args[1]);

  // Only inspect suspended generator scopes.
  if (!gen->is_suspended()) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  // Find the requested scope.
  int n = 0;
  ScopeIterator it(isolate, gen);
  for (; !it.Done() && n < index; it.Next()) {
    n++;
  }
  if (it.Done()) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  return *it.MaterializeScopeDetails();
}

static bool SetScopeVariableValue(ScopeIterator* it, int index,
                                  Handle<String> variable_name,
                                  DirectHandle<Object> new_value) {
  for (int n = 0; !it->Done() && n < index; it->Next()) {
    n++;
  }
  if (it->Done()) {
    return false;
  }
  return it->SetVariableValue(variable_name, new_value);
}

// Change variable value in closure or local scope
// args[0]: number or JsFunction: break id or function
// args[1]: number: scope index
// args[2]: string: variable name
// args[3]: object: new value
//
// Return true if success and false otherwise
RUNTIME_FUNCTION(Runtime_SetGeneratorScopeVariableValue) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  Handle<JSGeneratorObject> gen = args.at<JSGeneratorObject>(0);
  int index = NumberToInt32(args[1]);
  Handle<String> variable_name = args.at<String>(2);
  DirectHandle<Object> new_value = args.at(3);
  ScopeIterator it(isolate, gen);
  bool res = SetScopeVariableValue(&it, index, variable_name, new_value);
  return ReadOnlyRoots(isolate).boolean_value(res);
}

RUNTIME_FUNCTION(Runtime_GetBreakLocations) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  CHECK(isolate->debug()->is_active());
  DirectHandle<JSFunction> fun = args.at<JSFunction>(0);

  DirectHandle<SharedFunctionInfo> shared(fun->shared(), isolate);
  // Find the number of break points
  DirectHandle<Object> break_locations =
      Debug::GetSourceBreakLocations(isolate, shared);
  if (IsUndefined(*break_locations)) {
    return ReadOnlyRoots(isolate).undefined_value();
  }
  // Return array as JS array
  return *isolate->factory()->NewJSArrayWithElements(
      Cast<FixedArray>(break_locations));
}

// Returns the state of break on exceptions
// args[0]: boolean indicating uncaught exceptions
RUNTIME_FUNCTION(Runtime_IsBreakOnException) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  uint32_t type_arg = NumberToUint32(args[0]);

  ExceptionBreakType type = static_cast<ExceptionBreakType>(type_arg);
  bool result = isolate->debug()->IsBreakOnException(type);
  return Smi::FromInt(result);
}

// Clear all stepping set by PrepareStep.
RUNTIME_FUNCTION(Runtime_ClearStepping) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  CHECK(isolate->debug()->is_active());
  isolate->debug()->ClearStepping();
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugGetLoadedScriptIds) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());

  DirectHandle<FixedArray> instances;
  {
    DebugScope debug_scope(isolate->debug());
    // Fill the script objects.
    instances = isolate->debug()->GetLoadedScripts();
  }

  // Convert the script objects to proper JS objects.
  uint32_t instances_len = instances->ulength().value();
  for (uint32_t i = 0; i < instances_len; i++) {
    DirectHandle<Script> script(Cast<Script>(instances->get(i)), isolate);
    instances->set(i, Smi::FromInt(script->id()));
  }

  // Return result as a JS array.
  return *isolate->factory()->NewJSArrayWithElements(instances);
}

RUNTIME_FUNCTION(Runtime_FunctionGetInferredName) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());

  Tagged<Object> f = args[0];
  if (IsJSFunction(f)) {
    return Cast<JSFunction>(f)->shared()->inferred_name();
  }
  return ReadOnlyRoots(isolate).empty_string();
}

// Performs a GC.
// Presently, it only does a full GC.
RUNTIME_FUNCTION(Runtime_CollectGarbage) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  isolate->heap()->PreciseCollectAllGarbage(GCFlag::kNoFlags,
                                            GarbageCollectionReason::kRuntime);
  return ReadOnlyRoots(isolate).undefined_value();
}

namespace {

int ScriptLinePosition(Isolate* isolate, DirectHandle<Script> script,
                       int int_line) {
  if (int_line < 0) return -1;
  uint32_t line = static_cast<uint32_t>(int_line);

#if V8_ENABLE_WEBASSEMBLY
  if (script->type() == Script::Type::kWasm) {
    // Wasm positions are relative to the start of the module.
    return 0;
  }
#endif  // V8_ENABLE_WEBASSEMBLY

  Script::InitLineEnds(isolate, script);

  Tagged<FixedArray> line_ends_array = Cast<FixedArray>(script->line_ends());
  const uint32_t line_count = line_ends_array->ulength().value();

  if (line == 0) return 0;
  // If line == line_count, we return the first position beyond the last line.
  if (line > line_count) return -1;
  return Smi::ToInt(line_ends_array->get(line - 1)) + 1;
}

int ScriptLinePositionWithOffset(Isolate* isolate, DirectHandle<Script> script,
                                 int line, int offset) {
  if (line < 0 || offset < 0) return -1;

  if (line == 0 || offset == 0) {
    return ScriptLinePosition(isolate, script, line) + offset;
  }

  Script::PositionInfo info;
  if (!Script::GetPositionInfo(script, offset, &info,
                               Script::OffsetFlag::kNoOffset)) {
    return -1;
  }

  const int total_line = info.line + line;
  return ScriptLinePosition(isolate, script, total_line);
}

DirectHandle<Object> GetJSPositionInfo(DirectHandle<Script> script,
                                       int position,
                                       Script::OffsetFlag offset_flag,
                                       Isolate* isolate) {
  Script::PositionInfo info;
  if (!Script::GetPositionInfo(script, position, &info, offset_flag)) {
    return isolate->factory()->null_value();
  }

#if V8_ENABLE_WEBASSEMBLY
  const bool is_wasm_script = script->type() == Script::Type::kWasm;
#else
  const bool is_wasm_script = false;
#endif  // V8_ENABLE_WEBASSEMBLY
  DirectHandle<String> sourceText =
      is_wasm_script ? isolate->factory()->empty_string()
                     : isolate->factory()->NewSubString(
                           handle(Cast<String>(script->source()), isolate),
                           info.line_start, info.line_end);

  DirectHandle<JSObject> jsinfo =
      isolate->factory()->NewJSObject(isolate->object_function());

  JSObject::AddProperty(isolate, jsinfo, isolate->factory()->script_string(),
                        script, NONE);
  JSObject::AddProperty(isolate, jsinfo, isolate->factory()->position_string(),
                        direct_handle(Smi::FromInt(position), isolate), NONE);
  JSObject::AddProperty(isolate, jsinfo, isolate->factory()->line_string(),
                        direct_handle(Smi::FromInt(info.line), isolate), NONE);
  JSObject::AddProperty(isolate, jsinfo, isolate->factory()->column_string(),
                        direct_handle(Smi::FromInt(info.column), isolate),
                        NONE);
  JSObject::AddProperty(isolate, jsinfo,
                        isolate->factory()->sourceText_string(), sourceText,
                        NONE);

  return jsinfo;
}

DirectHandle<Object> ScriptLocationFromLine(Isolate* isolate,
                                            DirectHandle<Script> script,
                                            DirectHandle<Object> opt_line,
                                            DirectHandle<Object> opt_column,
                                            int32_t offset) {
  // Line and column are possibly undefined and we need to handle these cases,
  // additionally subtracting corresponding offsets.

  int32_t line = 0;
  if (!IsNullOrUndefined(*opt_line)) {
    CHECK(IsNumber(*opt_line));
    line = NumberToInt32(*opt_line) - script->line_offset();
  }

  int32_t column = 0;
  if (!IsNullOrUndefined(*opt_column)) {
    CHECK(IsNumber(*opt_column));
    column = NumberToInt32(*opt_column);
    if (line == 0) column -= script->column_offset();
  }

  int line_position =
      ScriptLinePositionWithOffset(isolate, script, line, offset);
  if (line_position < 0 || column < 0) return isolate->factory()->null_value();

  return GetJSPositionInfo(script, line_position + column,
                           Script::OffsetFlag::kNoOffset, isolate);
}

// Slow traversal over all scripts on the heap.
bool GetScriptById(Isolate* isolate, int needle, DirectHandle<Script>* result) {
  Script::Iterator iterator(isolate);
  for (Tagged<Script> script = iterator.Next(); !script.is_null();
       script = iterator.Next()) {
    if (script->id() == needle) {
      *result = direct_handle(script, isolate);
      return true;
    }
  }

  return false;
}

}  // namespace

// TODO(5530): Rename once conflicting function has been deleted.
RUNTIME_FUNCTION(Runtime_ScriptLocationFromLine2) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  int32_t scriptid = NumberToInt32(args[0]);
  DirectHandle<Object> opt_line = args.at(1);
  DirectHandle<Object> opt_column = args.at(2);
  int32_t offset = NumberToInt32(args[3]);

  DirectHandle<Script> script;
  CHECK(GetScriptById(isolate, scriptid, &script));

  return *ScriptLocationFromLine(isolate, script, opt_line, opt_column, offset);
}

// On function call, depending on circumstances, prepare for stepping in,
// or perform a side effect check.
RUNTIME_FUNCTION(Runtime_DebugOnFunctionCall) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  DirectHandle<JSFunction> fun = args.at<JSFunction>(0);
  DirectHandle<Object> receiver = args.at(1);
  if (isolate->debug()->needs_check_on_function_call()) {
    // Ensure that the callee will perform debug check on function call too.
    DirectHandle<SharedFunctionInfo> shared(fun->shared(), isolate);
    isolate->debug()->DeoptimizeFunction(shared);
    if (isolate->debug()->last_step_action() >= StepInto ||
        isolate->debug()->break_on_next_function_call()) {
      DCHECK_EQ(isolate->debug_execution_mode(), DebugInfo::kBreakpoints);
      isolate->debug()->PrepareStepIn(fun);
    }
    if (isolate->debug_execution_mode() == DebugInfo::kSideEffects &&
        !isolate->debug()->PerformSideEffectCheck(fun, receiver)) {
      return ReadOnlyRoots(isolate).exception();
    }
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

// Set one shot breakpoints for the suspended generator object.
RUNTIME_FUNCTION(Runtime_DebugPrepareStepInSuspendedGenerator) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  isolate->debug()->PrepareStepInSuspendedGenerator();
  return ReadOnlyRoots(isolate).undefined_value();
}

namespace {
DirectHandle<JSObject> MakeRangeObject(Isolate* isolate,
                                       const CoverageBlock& range) {
  Factory* factory = isolate->factory();

  DirectHandle<String> start_string = factory->InternalizeUtf8String("start");
  DirectHandle<String> end_string = factory->InternalizeUtf8String("end");
  DirectHandle<String> count_string = factory->InternalizeUtf8String("count");

  DirectHandle<JSObject> range_obj = factory->NewJSObjectWithNullProto();
  JSObject::AddProperty(isolate, range_obj, start_string,
                        factory->NewNumberFromInt(range.start), NONE);
  JSObject::AddProperty(isolate, range_obj, end_string,
                        factory->NewNumberFromInt(range.end), NONE);
  JSObject::AddProperty(isolate, range_obj, count_string,
                        factory->NewNumberFromUint(range.count), NONE);

  return range_obj;
}
}  // namespace

RUNTIME_FUNCTION(Runtime_DebugCollectCoverage) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  // Collect coverage data.
  std::unique_ptr<Coverage> coverage;
  if (isolate->is_best_effort_code_coverage()) {
    coverage = Coverage::CollectBestEffort(isolate);
  } else {
    coverage = Coverage::CollectPrecise(isolate);
  }
  Factory* factory = isolate->factory();
  // Turn the returned data structure into JavaScript.
  // Create an array of scripts.
  int num_scripts = static_cast<int>(coverage->size());
  // Prepare property keys.
  DirectHandle<FixedArray> scripts_array = factory->NewFixedArray(num_scripts);
  DirectHandle<String> script_string = factory->script_string();
  for (int i = 0; i < num_scripts; i++) {
    const auto& script_data = coverage->at(i);
    HandleScope inner_scope(isolate);

    std::vector<CoverageBlock> ranges;
    int num_functions = static_cast<int>(script_data.functions.size());
    for (int j = 0; j < num_functions; j++) {
      const auto& function_data = script_data.functions[j];
      ranges.emplace_back(function_data.start, function_data.end,
                          function_data.count);
      for (size_t k = 0; k < function_data.blocks.size(); k++) {
        const auto& block_data = function_data.blocks[k];
        ranges.emplace_back(block_data.start, block_data.end, block_data.count);
      }
    }

    int num_ranges = static_cast<int>(ranges.size());
    DirectHandle<FixedArray> ranges_array = factory->NewFixedArray(num_ranges);
    for (int j = 0; j < num_ranges; j++) {
      DirectHandle<JSObject> range_object = MakeRangeObject(isolate, ranges[j]);
      ranges_array->set(j, *range_object);
    }

    DirectHandle<JSArray> script_obj =
        factory->NewJSArrayWithElements(ranges_array, PACKED_ELEMENTS);
    JSObject::AddProperty(isolate, script_obj, script_string,
                          direct_handle(script_data.script->source(), isolate),
                          NONE);
    scripts_array->set(i, *script_obj);
  }
  return *factory->NewJSArrayWithElements(scripts_array, PACKED_ELEMENTS);
}

#if V8_ENABLE_WEBASSEMBLY
RUNTIME_FUNCTION(Runtime_DebugCollectWasmCoverage) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());

  // Collect Wasm coverage data.
  std::unique_ptr<Coverage> coverage = Coverage::CollectWasmData(isolate);

  Factory* factory = isolate->factory();
  // Turn the returned data structure into JavaScript.
  // Create an array of scripts.
  int num_scripts = static_cast<int>(coverage->size());
  DirectHandle<FixedArray> scripts_array = factory->NewFixedArray(num_scripts);
  for (int i = 0; i < num_scripts; i++) {
    HandleScope inner_scope(isolate);

    const CoverageScript& script_data = coverage->at(i);
    Handle<Script> script = script_data.script;
    DCHECK_EQ(script->type(), Script::Type::kWasm);
    const wasm::WasmModule* module = script->wasm_native_module()->module();

    std::vector<CoverageBlock> ranges;
    int num_functions = static_cast<int>(script_data.functions.size());
    int code_start =
        num_functions > 0
            ? module->functions[module->num_imported_functions].code.offset()
            : 0;
    for (int j = 0; j < num_functions; j++) {
      int function_index = module->num_imported_functions + j;
      uint32_t function_start_offset =
          module->functions[function_index].code.offset();
      const auto& function_data = script_data.functions[j];
      for (size_t k = 0; k < function_data.blocks.size(); k++) {
        const auto& block_data = function_data.blocks[k];
        ranges.emplace_back(
            function_start_offset - code_start + block_data.start,
            function_start_offset - code_start + block_data.end,
            block_data.count);
      }
    }

    int num_ranges = static_cast<int>(ranges.size());
    DirectHandle<FixedArray> ranges_array = factory->NewFixedArray(num_ranges);
    for (int j = 0; j < num_ranges; j++) {
      DirectHandle<JSObject> range_object = MakeRangeObject(isolate, ranges[j]);
      ranges_array->set(j, *range_object);
    }

    DirectHandle<JSArray> ranges_obj =
        factory->NewJSArrayWithElements(ranges_array, PACKED_ELEMENTS);
    scripts_array->set(i, *ranges_obj);
  }
  return *factory->NewJSArrayWithElements(scripts_array, PACKED_ELEMENTS);
}
#endif  // V8_ENABLE_WEBASSEMBLY

RUNTIME_FUNCTION(Runtime_DebugTogglePreciseCoverage) {
  SealHandleScope shs(isolate);
  bool enable = Cast<Boolean>(args[0])->ToBool(isolate);
  Coverage::SelectMode(isolate, enable ? debug::CoverageMode::kPreciseCount
                                       : debug::CoverageMode::kBestEffort);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugToggleBlockCoverage) {
  SealHandleScope shs(isolate);
  bool enable = Cast<Boolean>(args[0])->ToBool(isolate);
  Coverage::SelectMode(isolate, enable ? debug::CoverageMode::kBlockCount
                                       : debug::CoverageMode::kBestEffort);
  return ReadOnlyRoots(isolate).undefined_value();
}

extern uint64_t* gProgressCounter;
extern uint64_t gTargetProgress;
extern bool gRecordReplayAssertProgress;
extern int gRecordReplayCheckProgress;

// Define this to check preconditions for using record/replay opcodes.
//#define RECORD_REPLAY_CHECK_OPCODES

#ifdef RECORD_REPLAY_CHECK_OPCODES

extern bool RecordReplayHasRegisteredScript(Script script);

static inline bool RecordReplayBytecodeAllowed() {
  return IsMainThread()
      && (!recordreplay::AreEventsDisallowed() || recordreplay::HasDivergedFromRecording());
}

#else // !RECORD_REPLAY_CHECK_OPCODES

static inline bool RecordReplayHasRegisteredScript(Script script) {
  return true;
}

static inline bool RecordReplayBytecodeAllowed() {
  return true;
}

#endif // !RECORD_REPLAY_CHECK_OPCODES

// When gRecordReplayAssertProgress is set we keep track of all the progress
// made on the main thread and associate it with main-thread assertions using
// the recorder's assert data callbacks API. Each progress advancement is
// associated with a single 64 bit value encoding the script ID and location
// within that script of the function which executed.
static std::vector<uint64_t>* gProgressData;

// Buffer holding data most recently reported to the recorder.
static std::vector<uint64_t>* gReportedProgressData;

static inline uint64_t BuildScriptProgressEntry(Handle<JSFunction> fun) {
  int script_id = Script::cast(fun->shared().script()).id();
  int start_position = fun->shared().StartPosition();
  return (static_cast<uint64_t>(script_id) << 32) | static_cast<uint64_t>(start_position);
}

extern Handle<Script> GetScript(Isolate* isolate, int script_id);

static inline std::string GetScriptName(Handle<Script> script) {
  return script->name().IsString()
    ? String::cast(script->name()).ToCString().get()
    : "(anonymous script)";
}

std::string GetScriptLocationString(int script_id, int start_position) {
  Isolate* isolate = Isolate::Current();
  HandleScope scope(isolate);
  Handle<Script> script = GetScript(isolate, script_id);
  std::string script_name = GetScriptName(script);

  Script::PositionInfo info;
  Script::GetPositionInfo(script, start_position, &info, Script::WITH_OFFSET);

  std::ostringstream os;
  os << script_id << ":" << script_name << ":" << info.line + 1 << ":" << info.column;
  return os.str();
}

static std::string GetScriptProgressEntryString(uint64_t v) {
  int script_id = static_cast<int>(v >> 32);
  int start_position = static_cast<int>(v);

  return GetScriptLocationString(script_id, start_position);
}

// Produce a string explaining a JS mismatch during an Assert call when a C++
// mismatch was detected. It includes the first mismatching replayed PC (if
// there is any). That PC is the current PC minus the index of the mismatching
// replayed entry, since every PC update before the current Assert added one
// JS mismatch entry.
// See https://linear.app/replay/issue/RUN-2096#comment-a334b15f
static char* GetProgressMismatchMessage(size_t replayedIndex, uint64_t recordedEntry,
                                        uint64_t replayedEntry) {
  std::string recorded_text = recordedEntry
                                  ? GetScriptProgressEntryString(recordedEntry)
                                  : "<assertion>";
  std::string replayed_text = replayedEntry
                                  ? GetScriptProgressEntryString(replayedEntry)
                                  : "<assertion>";
  std::ostringstream os;
  os << "{ \"recorded\": \"" << recorded_text 
     << "\", \"replayed\": \"" << replayed_text
     << "\", \"progress\": " << (*gProgressCounter - replayedIndex);
  os << "\" }";
  
  return strdup(os.str().c_str());
}

void RecordReplayCallbackAssertGetData(void** pbuf, size_t* psize) {
  if (!IsMainThread() || !gProgressData) {
    *psize = 0;
    return;
  }

  if (gReportedProgressData) {
    delete gReportedProgressData;
  }
  gReportedProgressData = gProgressData;
  gProgressData = nullptr;
  *pbuf = &(*gReportedProgressData)[0];
  *psize = gReportedProgressData->size() * sizeof(uint64_t);
}

extern void RecordReplayDescribeAssertData(const char* text);

char* RecordReplayCallbackAssertOnDataMismatch(void* recorded_buf, size_t recorded_buf_size,
                                               void* replayed_buf, size_t replayed_buf_size) {
  const uint64_t* recorded = reinterpret_cast<const uint64_t*>(recorded_buf);
  size_t recorded_size = recorded_buf_size / sizeof(uint64_t);

  const uint64_t* replayed = reinterpret_cast<const uint64_t*>(replayed_buf);
  size_t replayed_size = replayed_buf_size / sizeof(uint64_t);

  for (size_t i = 0; i < std::min<size_t>(recorded_size, replayed_size); i++) {
    if (recorded[i] == replayed[i]) {
      std::string text = GetScriptProgressEntryString(recorded[i]);
      RecordReplayDescribeAssertData(text.c_str());
    } else {
      return GetProgressMismatchMessage(replayed_size - i - 1, recorded[i], replayed[i]);
    }
  }

  if (recorded_size < replayed_size) {
    // We are missing recorded entries. Report the first extra replayed entry.
    return GetProgressMismatchMessage(
      replayed_size - recorded_size - 1, 0, replayed[recorded_size]);
  }

  if (replayed_size < recorded_size) {
    // We are missing replayed entries. Report the first extra recorded entry.
    return GetProgressMismatchMessage(
      0, recorded[replayed_size], 0);
  }

  // Everything is equal.
  return GetProgressMismatchMessage(0, 0, 0);
}

void RecordReplayCallbackAssertDescribeData(void* buf, size_t buf_size) {
  const uint64_t* entries = reinterpret_cast<const uint64_t*>(buf);
  size_t size = buf_size / sizeof(uint64_t);

  for (size_t i = 0; i < size; i++) {
    std::string text = GetScriptProgressEntryString(entries[i]);
    RecordReplayDescribeAssertData(text.c_str());
  }
}

extern bool gRecordReplayHasCheckpoint;

extern void RecordReplayOnTargetProgressReached();
extern bool RecordReplayIsDivergentUserJSWithoutPause(
    const SharedFunctionInfo& shared);

static bool gHasPrintedStack = false;

RUNTIME_FUNCTION(Runtime_RecordReplayAssertExecutionProgress) {
  if (++*gProgressCounter == gTargetProgress) {
    RecordReplayOnTargetProgressReached();
  }

  if (gRecordReplayAssertProgress) {
    Handle<JSFunction> function = args.at<JSFunction>(0);

    if (!gProgressData) {
      gProgressData = new std::vector<uint64_t>();
    }
    gProgressData->push_back(BuildScriptProgressEntry(function));
  }

  if (gRecordReplayCheckProgress) {
    Handle<JSFunction> function = args.at<JSFunction>(0);

    Handle<SharedFunctionInfo> shared(function->shared(), isolate);
    Handle<Script> script(Script::cast(shared->script()), isolate);

    CHECK(RecordReplayBytecodeAllowed());
    CHECK(gRecordReplayHasCheckpoint);
    CHECK(RecordReplayHasRegisteredScript(*script));

    if (recordreplay::AreEventsDisallowed() && !recordreplay::HasDivergedFromRecording()) {
      // Print JS stack if user JS was executed non-deterministically
      // and we were not paused.
      if (!gHasPrintedStack) {  // Prevent flood.
        gHasPrintedStack = true;
        HandleScope scope(isolate);
        std::stringstream stack;
        isolate->PrintCurrentStackTrace(stack);

        recordreplay::Warning(
            "[RUN-1919] JS ExecutionProgress in non-deterministic user JS PC=%zu "
            "scriptId=%d @%s stack=%s",
            *gProgressCounter, script->id(),
            GetScriptLocationString(script->id(), shared->StartPosition())
                .c_str(),
            stack.str().c_str());
      }
    }
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_RecordReplayTargetProgressReached) {
  CHECK(*gProgressCounter == gTargetProgress);
  RecordReplayOnTargetProgressReached();
  return ReadOnlyRoots(isolate).undefined_value();
}

extern "C" void V8RecordReplayNotifyActivity();

RUNTIME_FUNCTION(Runtime_RecordReplayNotifyActivity) {
  V8RecordReplayNotifyActivity();
  return ReadOnlyRoots(isolate).undefined_value();
}

static std::string GetStackLocation(Isolate* isolate) {
  HandleScope scope(isolate);
  char location[1024];
  strcpy(location, "<no frame>");
  for (StackFrameIterator it(isolate); !it.done(); it.Advance()) {
    StackFrame* frame = it.frame();
    if (!frame->is_java_script()) {
      continue;
    }
    std::vector<FrameSummary> frames;
    CommonFrame::cast(frame)->Summarize(&frames);
    if (!frames.size()) {
      continue;
    }
    auto& summary = frames.back();
    CHECK(summary.IsJavaScript());
    auto const& js = summary.AsJavaScript();

    Handle<SharedFunctionInfo> shared(js.function()->shared(), isolate);

    // Sometimes the SharedFunctionInfo has what appears to be a bogus
    // script for an unknown reason. We check the positions of the function
    // to watch for this.
    if (!shared->StartPosition() && !shared->EndPosition()) {
      continue;
    }

    Handle<Script> script(Script::cast(shared->script()), isolate);

    if (script->id() == 0) {
      continue;
    }

    int source_position = js.SourcePosition();
    Script::PositionInfo info;
    Script::GetPositionInfo(script, source_position, &info, Script::WITH_OFFSET);

    if (script->name().IsUndefined()) {
      snprintf(location, sizeof(location), "<none>:%d:%d", info.line + 1, info.column);
    } else {
      std::unique_ptr<char[]> name = String::cast(script->name()).ToCString();
      snprintf(location, sizeof(location), "%s:%d:%d", name.get(), info.line + 1, info.column);
    }
    location[sizeof(location) - 1] = 0;
    break;
  }

  return std::string(location);
}

// Assertion and instrumentation site indexes embedded in bytecodes are offset
// by this value. This forces the bytecode emitter to always use four bytes to
// encode the index, so that bytecode offsets will be stable between recording
// and replaying (or different replays) even if the indexes themselves aren't.
static const int BytecodeSiteOffset = 1 << 16;

// Locations for each assertion site, filled in lazily.
struct AssertionSite {
  std::string desc_;
  int source_position_;
  std::string location_;
};
typedef std::vector<AssertionSite*> AssertionSiteVector;
static AssertionSiteVector* gAssertionSites;
static base::Mutex* gAssertionSitesMutex;

int RegisterAssertValueSite(const std::string& desc, int source_position) {
  base::MutexGuard lock(gAssertionSitesMutex);

  int index = (int)gAssertionSites->size();
  gAssertionSites->push_back(new AssertionSite({ desc, source_position, "" }));
  return index + BytecodeSiteOffset;
}

static inline AssertionSite& GetAssertValueSite(int32_t index) {
  index -= BytecodeSiteOffset;

  base::MutexGuard lock(gAssertionSitesMutex);

  CHECK(gAssertionSites && (size_t)index < gAssertionSites->size());
  AssertionSite* site = (*gAssertionSites)[index];
  return *site;
}

extern std::string RecordReplayBasicValueContents(Handle<Object> value);

RUNTIME_FUNCTION(Runtime_RecordReplayAssertValue) {
  CHECK(RecordReplayBytecodeAllowed());

  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  Handle<JSFunction> function = args.at<JSFunction>(0);
  int32_t index = NumberToInt32(args[1]);
  Handle<Object> value = args.at(2);

  Handle<Script> script(Script::cast(function->shared().script()), isolate);
  CHECK(RecordReplayHasRegisteredScript(*script));

  AssertionSite& site = GetAssertValueSite(index);

  if (!site.location_.length()) {
    Script::PositionInfo info;
    Script::GetPositionInfo(script, site.source_position_, &info, Script::WITH_OFFSET);

    char buf[1024];
    if (script->name().IsUndefined()) {
      snprintf(buf, sizeof(buf), "<none>:%d:%d", info.line + 1, info.column);
    } else {
      std::unique_ptr<char[]> name = String::cast(script->name()).ToCString();
      snprintf(buf, sizeof(buf), "%s:%d:%d", name.get(), info.line + 1, info.column);
    }
    buf[sizeof(buf) - 1] = 0;

    site.location_ = buf;
  }

  std::string contents = RecordReplayBasicValueContents(value);

  REPLAY_ASSERT(
      "JS %s Value=%s PC=%zu scriptId=%d @%s", site.desc_.c_str(),
      contents.c_str(), *gProgressCounter, script->id(),
      site.location_.c_str());

  if ((RecordReplayIsDivergentUserJSWithoutPause(function->shared())) ||
      (recordreplay::IsReplaying() && recordreplay::HadMismatch())) {
    // Print JS stack if user JS was executed non-deterministically
    // and we were not paused, or if we had a mismatch.
    if (!gHasPrintedStack) {  // Prevent flood.
      gHasPrintedStack = true;
      std::stringstream stack;
      isolate->PrintCurrentStackTrace(stack);

      recordreplay::Warning(
          "JS-Stack %s%s PC=%zu scriptId=%d @%s stack=%s", site.desc_.c_str(),
          RecordReplayIsDivergentUserJSWithoutPause(function->shared())
              ? " in non-deterministic user JS"
              : "",
          *gProgressCounter, script->id(), site.location_.c_str(),
          stack.str().c_str());
    }
  }

  return *value;
}

struct InstrumentationSite {
  const char* kind_ = nullptr;
  int source_position_ = 0;
  // The index of this site within its function.
  int function_index_ = 0;

  // Set on the first use of the instrumentation site.
  std::string function_id_;
};

typedef std::vector<InstrumentationSite> InstrumentationSiteVector;

// All instrumentation sites that have been registered, possibly on a non-main
// thread during background compilation tasks. Protected by gInstrumentationSitesMutex.
static InstrumentationSiteVector* gInstrumentationSites;
static base::Mutex* gInstrumentationSitesMutex;

// Prefix of gInstrumentationSites, main thread only. Used to avoid locking.
static InstrumentationSiteVector* gMainThreadInstrumentationSites;

// On the main thread, copy over any instrumentation sites that haven't
// been added to gMainThreadInstrumentationSites.
static void CopyMainThreadInstrumentationSites() {
  base::MutexGuard lock(gInstrumentationSitesMutex);
  for (size_t i = gMainThreadInstrumentationSites->size();
       i < gInstrumentationSites->size();
       i++) {
    gMainThreadInstrumentationSites->push_back((*gInstrumentationSites)[i]);
  }
}

int RegisterInstrumentationSite(const char* kind, int source_position,
                                int function_index) {
  InstrumentationSite site;
  site.kind_ = kind;
  site.source_position_ = source_position;
  site.function_index_ = function_index;

  base::MutexGuard lock(gInstrumentationSitesMutex);

  int index = (int)gInstrumentationSites->size();
  gInstrumentationSites->push_back(site);

  return index + BytecodeSiteOffset;
}

static InstrumentationSite& GetInstrumentationSite(const char* why, int index) {
  CHECK(IsMainThread());
  index -= BytecodeSiteOffset;
  if ((size_t)index >= gMainThreadInstrumentationSites->size()) {
    CopyMainThreadInstrumentationSites();
    if ((size_t)index >= gMainThreadInstrumentationSites->size()) {
      recordreplay::Diagnostic("BadInstrumentationSite %s %d %d",
                               why, index, gMainThreadInstrumentationSites->size());
      CHECK((size_t)index < gMainThreadInstrumentationSites->size());
    }
  }
  return (*gMainThreadInstrumentationSites)[index];
}

const char* InstrumentationSiteKind(int index) {
  return GetInstrumentationSite("Kind", index).kind_;
}

int InstrumentationSiteSourcePosition(int index) {
  return GetInstrumentationSite("SourcePosition", index).source_position_;
}

int InstrumentationSiteFunctionIndex(int index) {
  return GetInstrumentationSite("Rank", index).function_index_;
}

void RecordReplayInitInstrumentationState() {
  // These can't have static ctors/dtors...
  gAssertionSites = new AssertionSiteVector();
  gAssertionSitesMutex = new base::Mutex();
  gInstrumentationSites = new InstrumentationSiteVector();
  gInstrumentationSitesMutex = new base::Mutex();
  gMainThreadInstrumentationSites = new InstrumentationSiteVector();
}

extern void RecordReplayInstrument(const char* kind, const char* function, int function_index);

// Enable to dump locations of each function to stderr.
static bool gDumpFunctionLocations;

std::string GetRecordReplayFunctionId(Handle<SharedFunctionInfo> shared) {
  Script script = Script::cast(shared->script());

  std::ostringstream os;

  // When recording/replaying we use a function ID we can parse to a script
  // and source location later.
  os << script.id() << ":" << shared->StartPosition();

  if (gDumpFunctionLocations) {
    std::unique_ptr<char[]> url;
    if (!script.name().IsUndefined()) {
      url = String::cast(script.name()).ToCString();
    }

    Script::PositionInfo info;
    Handle<Script> handleScript(script, Isolate::Current());
    Script::GetPositionInfo(handleScript, shared->StartPosition(), &info, Script::WITH_OFFSET);
    recordreplay::Print("FunctionId %s -> %s:%d:%d",
                        os.str().c_str(), url.get() ? url.get() : "<none>",
                        info.line + 1, info.column);
  }

  return os.str();
}

void ParseRecordReplayFunctionId(const std::string& function_id,
                                 int* script_id, int* source_position) {
  const char* raw = function_id.c_str();
  *script_id = atoi(raw);
  *source_position = atoi(strchr(raw, ':') + 1);
}

static inline void OnInstrumentation(Isolate* isolate,
                                     Handle<JSFunction> function, int32_t index) {
  CHECK(RecordReplayBytecodeAllowed());

  Handle<Script> script(Script::cast(function->shared().script()), isolate);
  CHECK(RecordReplayHasRegisteredScript(*script));

  InstrumentationSite& site = GetInstrumentationSite("Callback", index);

  if (!site.function_id_.length()) {
    Handle<SharedFunctionInfo> shared(function->shared(), isolate);
    site.function_id_ = GetRecordReplayFunctionId(shared);
  }

  RecordReplayInstrument(site.kind_, site.function_id_.c_str(),
                         site.function_index_);
}

extern bool gRecordReplayInstrumentationEnabled;

RUNTIME_FUNCTION(Runtime_RecordReplayInstrumentation) {
  if (!gRecordReplayInstrumentationEnabled) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<JSFunction> function = args.at<JSFunction>(0);
  int32_t index = NumberToInt32(args[1]);

  OnInstrumentation(isolate, function, index);

  return ReadOnlyRoots(isolate).undefined_value();
}

extern int RecordReplayObjectId(v8::Isolate* isolate, Local<v8::Context> cx,
                                v8::Local<v8::Value> object, bool allow_create);

static int gCurrentGeneratorId;

int RecordReplayCurrentGeneratorIdRaw() {
  return gCurrentGeneratorId;
}

RUNTIME_FUNCTION(Runtime_RecordReplayInstrumentationGenerator) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  Handle<JSFunction> function = args.at<JSFunction>(0);
  int32_t index = NumberToInt32(args[1]);
  Handle<Object> generator_object = args.at(2);

  // Note: RecordReplayObjectId calls have to occur in the same places when
  // replaying (regardless of whether instrumentation is enabled) so that objects
  // will be assigned consistent IDs.
  CHECK(!gCurrentGeneratorId);
  v8::Isolate* v8_isolate = (v8::Isolate*) isolate;
  gCurrentGeneratorId = RecordReplayObjectId(v8_isolate, v8_isolate->GetCurrentContext(),
                                             v8::Utils::ToLocal(generator_object),
                                             /* allow_create */ true);

  if (gRecordReplayInstrumentationEnabled) {
    OnInstrumentation(isolate, function, index);
  }

  gCurrentGeneratorId = 0;

  return ReadOnlyRoots(isolate).undefined_value();
}

static Handle<Object>* gCurrentReturnValue;

RUNTIME_FUNCTION(Runtime_RecordReplayInstrumentationReturn) {
  if (!gRecordReplayInstrumentationEnabled) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  Handle<JSFunction> function = args.at<JSFunction>(0);
  int32_t index = NumberToInt32(args[1]);
  Handle<Object> return_value = args.at(2);

  gCurrentReturnValue = &return_value;

  OnInstrumentation(isolate, function, index);

  gCurrentReturnValue = nullptr;

  return ReadOnlyRoots(isolate).undefined_value();
}

extern "C" bool V8RecordReplayCurrentReturnValue(v8::Local<v8::Value>* object) {
  if (gCurrentReturnValue) {
    *object = v8::Utils::ToLocal(*gCurrentReturnValue);
    return true;
  }
  return false;
}

RUNTIME_FUNCTION(Runtime_RecordReplayTrackObjectId) {
  DCHECK_EQ(1, args.length());
  Handle<Object> value = args.at(0);

  v8::Isolate* v8_isolate = (v8::Isolate*) isolate;
  RecordReplayObjectId(v8_isolate, v8_isolate->GetCurrentContext(),
                       v8::Utils::ToLocal(value),
                       /* allow_create */ true);

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_IncBlockCounter) {
  UNREACHABLE();  // Never called. See the IncBlockCounter builtin instead.
}

RUNTIME_FUNCTION(Runtime_DebugAsyncFunctionSuspended) {
  DCHECK_EQ(4, args.length());
  HandleScope scope(isolate);
  DirectHandle<JSPromise> promise = args.at<JSPromise>(0);
  DirectHandle<JSPromise> outer_promise = args.at<JSPromise>(1);
  DirectHandle<JSFunction> reject_handler = args.at<JSFunction>(2);
  DirectHandle<JSGeneratorObject> generator = args.at<JSGeneratorObject>(3);

  // Allocate the throwaway promise and fire the appropriate init
  // hook for the throwaway promise (passing the {promise} as its
  // parent).
  DirectHandle<JSPromise> throwaway =
      isolate->factory()->NewJSPromiseWithoutHook();
  isolate->OnAsyncFunctionSuspended(throwaway, promise);

  // The Promise will be thrown away and not handled, but it
  // shouldn't trigger unhandled reject events as its work is done
  throwaway->set_has_handler(true);

  // Enable proper debug support for promises.
  if (isolate->debug()->is_active()) {
    Object::SetProperty(isolate, reject_handler,
                        isolate->factory()->promise_forwarding_handler_symbol(),
                        isolate->factory()->true_value(),
                        StoreOrigin::kMaybeKeyed,
                        Just(ShouldThrow::kThrowOnError))
        .Check();

    // Mark the dependency to {outer_promise} in case the {throwaway}
    // Promise is found on the Promise stack
    Object::SetProperty(isolate, throwaway,
                        isolate->factory()->promise_handled_by_symbol(),
                        outer_promise, StoreOrigin::kMaybeKeyed,
                        Just(ShouldThrow::kThrowOnError))
        .Check();

    DirectHandle<WeakFixedArray> awaited_by_holder(
        isolate->factory()->NewWeakFixedArray(1));
    awaited_by_holder->set(0, MakeWeak(*generator));
    Object::SetProperty(isolate, promise,
                        isolate->factory()->promise_awaited_by_symbol(),
                        awaited_by_holder, StoreOrigin::kMaybeKeyed,
                        Just(ShouldThrow::kThrowOnError))
        .Check();
  }

  return *throwaway;
}

RUNTIME_FUNCTION(Runtime_DebugPromiseThen) {
  DCHECK_EQ(1, args.length());
  HandleScope scope(isolate);
  DirectHandle<JSReceiver> promise = args.at<JSReceiver>(0);
  if (IsJSPromise(*promise)) {
    isolate->OnPromiseThen(Cast<JSPromise>(promise));
  }
  return *promise;
}

RUNTIME_FUNCTION(Runtime_LiveEditPatchScript) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  DirectHandle<JSFunction> script_function = args.at<JSFunction>(0);
  Handle<String> new_source = args.at<String>(1);

  Handle<Script> script(Cast<Script>(script_function->shared()->script()),
                        isolate);
  v8::debug::LiveEditResult result;
  isolate->debug()->SetScriptSource(script, new_source, /* preview */ false,
                                    /* allow_top_frame_live_editing */ false,
                                    &result);
  switch (result.status) {
    case v8::debug::LiveEditResult::COMPILE_ERROR:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: COMPILE_ERROR"));
    case v8::debug::LiveEditResult::BLOCKED_BY_RUNNING_GENERATOR:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: BLOCKED_BY_RUNNING_GENERATOR"));
    case v8::debug::LiveEditResult::BLOCKED_BY_ACTIVE_FUNCTION:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: BLOCKED_BY_ACTIVE_FUNCTION"));
    case v8::debug::LiveEditResult::BLOCKED_BY_TOP_LEVEL_ES_MODULE_CHANGE:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: BLOCKED_BY_TOP_LEVEL_ES_MODULE_CHANGE"));
    case v8::debug::LiveEditResult::FEATURE_DISABLED:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: FEATURE_DISABLED"));
    case v8::debug::LiveEditResult::OK:
      return ReadOnlyRoots(isolate).undefined_value();
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_ProfileCreateSnapshotDataBlob) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());

  // Used only by the test/memory/Memory.json benchmark. This creates a snapshot
  // blob and outputs various statistics around it.

  DCHECK(v8_flags.profile_deserialization && v8_flags.serialization_statistics);

  DisableEmbeddedBlobRefcounting();

  static constexpr char* kNoEmbeddedSource = nullptr;
  // We use this flag to tell the serializer not to finalize/seal RO space -
  // this already happened after deserializing the main Isolate.
  static constexpr Snapshot::SerializerFlags kSerializerFlags =
      Snapshot::SerializerFlag::kAllowActiveIsolateForTesting;
  v8::StartupData blob = CreateSnapshotDataBlobInternal(
      v8::SnapshotCreator::FunctionCodeHandling::kClear, kNoEmbeddedSource,
      kSerializerFlags);
  delete[] blob.data;

  // Track the embedded blob size as well.
  {
    i::EmbeddedData d = i::EmbeddedData::FromBlob(isolate);
    PrintF("Embedded blob is %d bytes\n",
           static_cast<int>(d.code_size() + d.data_size()));
  }

  FreeCurrentEmbeddedBlob();

  return ReadOnlyRoots(isolate).undefined_value();
}

}  // namespace internal
}  // namespace v8
