#include "base_object.h"
#include "env-inl.h"
#include "memory_tracker-inl.h"
#include "node_messaging.h"
#include "node_deferred_finalization.h"
#include "node_realm-inl.h"

namespace node {

using v8::Context;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Just;
using v8::JustVoid;
using v8::Local;
using v8::Maybe;
using v8::Object;
using v8::Value;
using v8::ValueDeserializer;
using v8::WeakCallbackInfo;
using v8::WeakCallbackType;

BaseObject::BaseObject(Realm* realm, Local<Object> object)
    : persistent_handle_(realm->isolate(), object), realm_(realm) {
  CHECK_EQ(false, object.IsEmpty());
  CHECK_GE(object->InternalFieldCount(), BaseObject::kInternalFieldCount);
  SetInternalFields(realm->isolate_data(), object, static_cast<void*>(this));
  realm->TrackBaseObject(this);
}

BaseObject::~BaseObject() {
  realm()->UntrackBaseObject(this);
  if (record_replay_id_ != 0) {
    env()->deferred_finalization()->Untrack(record_replay_id_);
  }

  if (has_pointer_data()) [[unlikely]] {
    PointerData* metadata = pointer_data();
    CHECK_EQ(metadata->strong_ptr_count, 0);
    metadata->self = nullptr;
    if (metadata->weak_ptr_count == 0) delete metadata;
  }

  if (persistent_handle_.IsEmpty()) {
    // This most likely happened because the weak callback below cleared it.
    return;
  }

  {
    HandleScope handle_scope(realm()->isolate());
    object()->SetAlignedPointerInInternalField(BaseObject::kSlot, nullptr);
  }
}

void BaseObject::MakeWeak() {
  if (has_pointer_data()) {
    pointer_data()->wants_weak_jsobj = true;
    if (pointer_data()->strong_ptr_count > 0) return;
  }

  // A tracked object stays alive when replaying until the recording shows
  // that the GC collected it, see RecordReplayTrack. It still counts as weak,
  // e.g. for IsWeakOrDetached().
  if (record_replay_id_ != 0 && v8::recordreplay::IsReplaying()) {
    pointer_data()->wants_weak_jsobj = true;
    return;
  }

  persistent_handle_.SetWeak(
      this,
      [](const WeakCallbackInfo<BaseObject>& data) {
        BaseObject* obj = data.GetParameter();
        // Clear the persistent handle so that ~BaseObject() doesn't attempt
        // to mess with internal fields, since the JS object may have
        // transitioned into an invalid state.
        // Refs: https://github.com/nodejs/node/issues/18897
        obj->persistent_handle_.Reset();
        CHECK_IMPLIES(obj->has_pointer_data(),
                      obj->pointer_data()->strong_ptr_count == 0);
        if (obj->record_replay_id_ != 0) {
          // The GC runs at a non-deterministic point, so the cleanup runs
          // from the next DeferredFinalization::Poll instead.
          obj->env()->deferred_finalization()->OnCollected(
              obj->record_replay_id_);
          return;
        }
        obj->OnGCCollect();
      },
      WeakCallbackType::kParameter);
}

void BaseObject::RecordReplayTrack(const char* label) {
  CHECK_EQ(record_replay_id_, 0);
  CHECK(!persistent_handle_.IsWeak());
  record_replay_id_ = env()->deferred_finalization()->Track(this, label);
}

bool BaseObject::IsRecordReplayTracked() const {
  return record_replay_id_ != 0;
}

void BaseObject::RecordReplayFinalize() {
  // When recording, the weak callback reset the handle. When replaying, the
  // object was kept alive instead: detach it from the JS object, which nothing
  // reaches anymore, as ~BaseObject() would, and reset the handle like the
  // weak callback did when recording.
  if (!persistent_handle_.IsEmpty()) {
    HandleScope handle_scope(env()->isolate());
    object()->SetAlignedPointerInInternalField(BaseObject::kSlot, nullptr);
    persistent_handle_.Reset();
  }
  CHECK_IMPLIES(has_pointer_data(), pointer_data()->strong_ptr_count == 0);
  OnGCCollect();
}
void BaseObject::LazilyInitializedJSTemplateConstructor(
    const FunctionCallbackInfo<Value>& args) {
  DCHECK(args.IsConstructCall());
  CHECK_GE(args.This()->InternalFieldCount(), BaseObject::kInternalFieldCount);
  Environment* env = Environment::GetCurrent(args);
  DCHECK_NOT_NULL(env);
  SetInternalFields(env->isolate_data(), args.This(), nullptr);
}

Local<FunctionTemplate> BaseObject::MakeLazilyInitializedJSTemplate(
    Environment* env, int internal_field_count) {
  return MakeLazilyInitializedJSTemplate(env->isolate_data(),
                                         internal_field_count);
}

Local<FunctionTemplate> BaseObject::MakeLazilyInitializedJSTemplate(
    IsolateData* isolate_data, int internal_field_count) {
  Local<FunctionTemplate> t = NewFunctionTemplate(
      isolate_data->isolate(), LazilyInitializedJSTemplateConstructor);
  t->InstanceTemplate()->SetInternalFieldCount(internal_field_count);
  return t;
}

BaseObject::TransferMode BaseObject::GetTransferMode() const {
  return TransferMode::kDisallowCloneAndTransfer;
}

std::unique_ptr<worker::TransferData> BaseObject::TransferForMessaging() {
  return {};
}

std::unique_ptr<worker::TransferData> BaseObject::CloneForMessaging() const {
  return {};
}

Maybe<std::vector<BaseObjectPtr<BaseObject>>> BaseObject::NestedTransferables()
    const {
  return Just(std::vector<BaseObjectPtr<BaseObject>>{});
}

Maybe<void> BaseObject::FinalizeTransferRead(Local<Context> context,
                                             ValueDeserializer* deserializer) {
  return JustVoid();
}

BaseObject::PointerData* BaseObject::pointer_data() {
  if (!has_pointer_data()) {
    PointerData* metadata = new PointerData();
    metadata->wants_weak_jsobj = persistent_handle_.IsWeak();
    metadata->self = this;
    pointer_data_ = metadata;
  }
  CHECK(has_pointer_data());
  return pointer_data_;
}

void BaseObject::decrease_refcount() {
  CHECK(has_pointer_data());
  PointerData* metadata = pointer_data();
  CHECK_GT(metadata->strong_ptr_count, 0);
  unsigned int new_refcount = --metadata->strong_ptr_count;
  if (new_refcount == 0) {
    if (metadata->is_detached) {
      OnGCCollect();
    } else if (metadata->wants_weak_jsobj && !persistent_handle_.IsEmpty()) {
      MakeWeak();
    }
  }
}

void BaseObject::increase_refcount() {
  unsigned int prev_refcount = pointer_data()->strong_ptr_count++;
  if (prev_refcount == 0 && !persistent_handle_.IsEmpty())
    persistent_handle_.ClearWeak();
}

void BaseObject::DeleteMe() {
  if (has_pointer_data() && pointer_data()->strong_ptr_count > 0) {
    return Detach();
  }
  delete this;
}

bool BaseObject::IsDoneInitializing() const {
  return true;
}

Local<Object> BaseObject::WrappedObject() const {
  return object();
}

bool BaseObject::IsNotIndicativeOfMemoryLeakAtExit() const {
  return IsWeakOrDetached();
}

void BaseObjectList::Cleanup() {
  while (!IsEmpty()) {
    BaseObject* bo = PopFront();
    bo->DeleteMe();
  }
}

void BaseObjectList::MemoryInfo(MemoryTracker* tracker) const {
  for (auto bo : *this) {
    if (bo->IsDoneInitializing()) {
      // TODO(addaleax): Add weak edges instead of no edges once
      // https://github.com/v8/v8/commit/e37cadf1143a8c5bbe44c0408186b5a26cc23863
      // is available for us
      tracker->Track(
          bo, bo->persistent().IsWeak() ? MemoryTracker::kWeakEdge : nullptr);
    }
  }
}

const char* const MemoryTracker::kWeakEdge = "<MemoryTracker::kWeakEdge>";

}  // namespace node
