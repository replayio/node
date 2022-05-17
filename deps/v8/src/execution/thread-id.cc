// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/thread-id.h"
#include "src/base/lazy-instance.h"
#include "src/base/platform/platform.h"

namespace v8 {
namespace internal {

namespace {

//thread_local int thread_id = 0;

std::atomic<int> next_thread_id{1};

}  // namespace

// FIXME using thread_local runs into problems when replaying, possibly related to
// process forking.
static int& GetThreadId() {
  static pthread_key_t key;
  if (!key) {
    pthread_key_create(&key, nullptr);
  }

  int* v = (int*)pthread_getspecific(key);
  if (!v) {
    v = new int(0);
    pthread_setspecific(key, v);
  }
  return *v;
}

#define thread_id GetThreadId()

// static
ThreadId ThreadId::TryGetCurrent() {
  return thread_id == 0 ? Invalid() : ThreadId(thread_id);
}

// static
int ThreadId::GetCurrentThreadId() {
  if (thread_id == 0) {
    thread_id = next_thread_id.fetch_add(1);
    CHECK_LE(1, thread_id);
  }
  return thread_id;
}

}  // namespace internal
}  // namespace v8
