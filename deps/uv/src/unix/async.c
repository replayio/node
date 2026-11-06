/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* This file contains both the uv__async internal infrastructure and the
 * user-facing uv_async_t functions.
 */

#include "uv.h"
#include "internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>  /* sched_yield() */

#ifdef __linux__
#include <sys/eventfd.h>
#endif

static void uv__async_send(uv_loop_t* loop);
static int uv__async_start(uv_loop_t* loop);
static void uv__cpu_relax(void);

extern void V8RecordReplayAssert(const char* format, ...);
extern size_t V8RecordReplayCreateOrderedLock(const char* name);
extern void V8RecordReplayOrderedLock(int lock);
extern void V8RecordReplayOrderedUnlock(int lock);


int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;

  err = uv__async_start(loop);
  if (err)
    return err;

  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  handle->async_cb = async_cb;
  handle->pending = 0;
  handle->u.fd = 0; /* This will be used as a busy flag. */
  handle->ordered_lock_id = V8RecordReplayCreateOrderedLock("uv_async_t");

  uv__queue_insert_tail(&loop->async_handles, &handle->queue);
  uv__handle_start(handle);

  return 0;
}


int uv_async_send(uv_async_t* handle) {
  _Atomic int* pending;
  _Atomic int* busy;

  pending = (_Atomic int*) &handle->pending;
  busy = (_Atomic int*) &handle->u.fd;

  V8RecordReplayOrderedLock(handle->ordered_lock_id);
  /* Do a cheap read first. */
  if (atomic_load_explicit(pending, memory_order_relaxed) != 0) {
    V8RecordReplayOrderedUnlock(handle->ordered_lock_id);
    return 0;
  }

  /* Set the loop to busy. */
  atomic_fetch_add(busy, 1);

  /* Wake up the other thread's event loop. */
  if (atomic_exchange(pending, 1) == 0)
    uv__async_send(handle->loop);

  /* Set the loop to not-busy. */
  atomic_fetch_add(busy, -1);

  V8RecordReplayOrderedUnlock(handle->ordered_lock_id);
  return 0;
}


/* Wait for the busy flag to clear before closing.
 * Only call this from the event loop thread. */
static void uv__async_spin(uv_async_t* handle) {
  _Atomic int* pending;
  _Atomic int* busy;
  int i;

  pending = (_Atomic int*) &handle->pending;
  busy = (_Atomic int*) &handle->u.fd;

  V8RecordReplayOrderedLock(handle->ordered_lock_id);

  /* Set the pending flag first, so no new events will be added by other
   * threads after this function returns. */
  atomic_store(pending, 1);

  for (;;) {
    /* 997 is not completely chosen at random. It's a prime number, acyclic by
     * nature, and should therefore hopefully dampen sympathetic resonance.
     */
    for (i = 0; i < 997; i++) {
      if (atomic_load(busy) == 0)
        goto done;

      /* Other thread is busy with this handle, spin until it's done. */
      uv__cpu_relax();
    }

    /* Yield the CPU. We may have preempted the other thread while it's
     * inside the critical section and if it's running on the same CPU
     * as us, we'll just burn CPU cycles until the end of our time slice.
     */
    sched_yield();
  }

done:
  V8RecordReplayOrderedUnlock(handle->ordered_lock_id);

  V8RecordReplayAssert("uv__async_spin");
}


void uv__async_close(uv_async_t* handle) {
  uv__async_spin(handle);
  uv__queue_remove(&handle->queue);
  uv__handle_stop(handle);
}


static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  char buf[1024];
  ssize_t r;
  struct uv__queue queue;
  struct uv__queue* q;
  uv_async_t* h;
  _Atomic int *pending;
  int was_pending;

  V8RecordReplayAssert("uv__async_io");

  assert(w == &loop->async_io_watcher);

  for (;;) {
    r = read(w->fd, buf, sizeof(buf));

    //V8RecordReplayAssert("uv__async_io #1 %d", r);

    if (r == sizeof(buf))
      continue;

    if (r != -1)
      break;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

    if (errno == EINTR)
      continue;

    abort();
  }

  uv__queue_move(&loop->async_handles, &queue);
  while (!uv__queue_empty(&queue)) {
    q = uv__queue_head(&queue);
    h = uv__queue_data(q, uv_async_t, queue);
    V8RecordReplayAssert("uv__async_io #2");

    uv__queue_remove(q);
    uv__queue_insert_tail(&loop->async_handles, q);

    /* Atomically fetch and clear pending flag */
    pending = (_Atomic int*) &h->pending;
    /* uv_async_send sets this flag under the handle's ordered lock, so clear it
     * under the lock too: which of the two got there first then follows the
     * recorded order of the lock rather than the threads' timing. The callback
     * runs outside the lock, since it may send to this handle itself. */
    V8RecordReplayOrderedLock(h->ordered_lock_id);
    was_pending = atomic_exchange(pending, 0);
    V8RecordReplayOrderedUnlock(h->ordered_lock_id);
    if (was_pending == 0) {
      V8RecordReplayAssert("uv__async_io #3");
      continue;  /* Not pending. */
    }

    V8RecordReplayAssert("uv__async_io #4");

    if (h->async_cb == NULL)
      continue;

    h->async_cb(h);
  }

  V8RecordReplayAssert("uv__async_io Done");
}


static void uv__async_send(uv_loop_t* loop) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;

  buf = "";
  len = 1;
  fd = loop->async_wfd;

#if defined(__linux__)
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    fd = loop->async_io_watcher.fd;  /* eventfd */
  }
#endif

  do
    r = write(fd, buf, len);
  while (r == -1 && errno == EINTR);

  if (r == len)
    return;

  if (r == -1)
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;

  abort();
}


static int uv__async_start(uv_loop_t* loop) {
  int pipefd[2];
  int err;

  if (loop->async_io_watcher.fd != -1)
    return 0;

#ifdef __linux__
  err = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (err < 0)
    return UV__ERR(errno);

  pipefd[0] = err;
  pipefd[1] = -1;
#else
  err = uv__make_pipe(pipefd, UV_NONBLOCK_PIPE);
  if (err < 0)
    return err;
#endif

  uv__io_init(&loop->async_io_watcher, uv__async_io, pipefd[0]);
  uv__io_start(loop, &loop->async_io_watcher, POLLIN);
  loop->async_wfd = pipefd[1];

  return 0;
}


void uv__async_stop(uv_loop_t* loop) {
  struct uv__queue queue;
  struct uv__queue* q;
  uv_async_t* h;

  if (loop->async_io_watcher.fd == -1)
    return;

  /* Make sure no other thread is accessing the async handle fd after the loop
   * cleanup.
   */
  uv__queue_move(&loop->async_handles, &queue);
  while (!uv__queue_empty(&queue)) {
    q = uv__queue_head(&queue);
    h = uv__queue_data(q, uv_async_t, queue);

    uv__queue_remove(q);
    uv__queue_insert_tail(&loop->async_handles, q);

    uv__async_spin(h);
  }

  if (loop->async_wfd != -1) {
    if (loop->async_wfd != loop->async_io_watcher.fd)
      uv__close(loop->async_wfd);
    loop->async_wfd = -1;
  }

  uv__io_stop(loop, &loop->async_io_watcher, POLLIN);
  uv__close(loop->async_io_watcher.fd);
  loop->async_io_watcher.fd = -1;
}


int uv__async_fork(uv_loop_t* loop) {
  struct uv__queue queue;
  struct uv__queue* q;
  uv_async_t* h;

  if (loop->async_io_watcher.fd == -1) /* never started */
    return 0;

  uv__queue_move(&loop->async_handles, &queue);
  while (!uv__queue_empty(&queue)) {
    q = uv__queue_head(&queue);
    h = uv__queue_data(q, uv_async_t, queue);

    uv__queue_remove(q);
    uv__queue_insert_tail(&loop->async_handles, q);

    /* The state of any thread that set pending is now likely corrupt in this
     * child because the user called fork, so just clear these flags and move
     * on. Calling most libc functions after `fork` is declared to be undefined
     * behavior anyways, unless async-signal-safe, for multithreaded programs
     * like libuv, and nothing interesting in pthreads is async-signal-safe.
     */
    h->pending = 0;
    /* This is the busy flag, and we just abruptly lost all other threads. */
    h->u.fd = 0;
  }

  /* Recreate these, since they still exist, but belong to the wrong pid now. */
  if (loop->async_wfd != -1) {
    if (loop->async_wfd != loop->async_io_watcher.fd)
      uv__close(loop->async_wfd);
    loop->async_wfd = -1;
  }

  uv__io_stop(loop, &loop->async_io_watcher, POLLIN);
  uv__close(loop->async_io_watcher.fd);
  loop->async_io_watcher.fd = -1;

  return uv__async_start(loop);
}


static void uv__cpu_relax(void) {
#if defined(__i386__) || defined(__x86_64__)
  __asm__ __volatile__ ("rep; nop" ::: "memory");  /* a.k.a. PAUSE */
#elif (defined(__arm__) && __ARM_ARCH >= 7) || defined(__aarch64__)
  __asm__ __volatile__ ("yield" ::: "memory");
#elif (defined(__ppc__) || defined(__ppc64__)) && defined(__APPLE__)
  __asm volatile ("" : : : "memory");
#elif !defined(__APPLE__) && (defined(__powerpc64__) || defined(__ppc64__) || defined(__PPC64__))
  __asm__ __volatile__ ("or 1,1,1; or 2,2,2" ::: "memory");
#endif
}
