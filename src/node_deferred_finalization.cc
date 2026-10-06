#include "node_deferred_finalization.h"

#include "async_wrap.h"
#include "env-inl.h"
#include "node.h"

namespace node {
namespace recordreplay {

namespace {

constexpr uintptr_t kDestroyedAsyncIds = 1 << 0;

template <typename T>
void RecordReplayList(const char* why, std::vector<T>* list) {
  list->resize(v8::recordreplay::RecordReplayValue(why, list->size()));
  v8::recordreplay::RecordReplayBytes(why, list->data(),
                                      list->size() * sizeof(T));
}

}  // anonymous namespace

DeferredFinalization::DeferredFinalization(Environment* env) : env_(env) {
  if (!v8::recordreplay::IsRecordingOrReplaying()) return;
  polling_ = true;
  env_->isolate()->AddMicrotasksCompletedCallback(MicrotasksCompleted, this);
}

DeferredFinalization::~DeferredFinalization() {
  if (!polling_) return;
  env_->isolate()->RemoveMicrotasksCompletedCallback(MicrotasksCompleted, this);
}

bool DeferredFinalization::Enabled(const char* label) {
  return v8::recordreplay::IsRecordingOrReplaying("deferred-finalization",
                                                   label);
}

bool DeferredFinalization::AddDestroyedAsyncId(double async_id) {
  if (!Enabled("AsyncWrap::EmitDestroy")) return false;
  // Runs inside the GC, so this only notes the id for the next poll. When
  // replaying, the recording names the ids to deliver.
  if (v8::recordreplay::IsRecording()) {
    destroyed_async_ids_.push_back(async_id);
  }
  return true;
}

bool DeferredFinalization::ShouldPoll() const {
  // Destroys are only noted while destroy hooks are enabled, which is JS state
  // and so the same on both sides. The poll records nothing otherwise.
  return env_->async_hooks()->fields()[AsyncHooks::kDestroy] != 0 &&
         Enabled("AsyncWrap::EmitDestroy");
}

void DeferredFinalization::Poll() {
  if (!ShouldPoll() || !AreEventsRecorded()) return;

  uintptr_t flags = 0;
  if (v8::recordreplay::IsRecording()) {
    if (!destroyed_async_ids_.empty()) flags |= kDestroyedAsyncIds;
  }
  flags = v8::recordreplay::RecordReplayValue("DeferredFinalization.poll",
                                              flags);

  if (flags & kDestroyedAsyncIds) {
    std::vector<double> ids;
    ids.swap(destroyed_async_ids_);
    RecordReplayList("DeferredFinalization.poll destroyed async ids", &ids);
    // Events are allowed here, so these take the regular path: onto the
    // destroy list, delivered from an immediate.
    for (double async_id : ids) {
      AsyncWrap::EmitDestroy(env_, async_id);
    }
  }
}

void DeferredFinalization::MicrotasksCompleted(v8::Isolate* isolate,
                                               void* data) {
  static_cast<DeferredFinalization*>(data)->Poll();
}

}  // namespace recordreplay
}  // namespace node
