/*
 * Record/Replay hooks. The V8RecordReplay* functions are exported by the
 * embedding V8 binary; binaries without V8, e.g. the openssl CLI, leave them
 * NULL.
 *
 * The recorder only replays the order in which threads take ordered locks.
 * Lock-free accesses to shared state are ordered here as well, as the values
 * they see decide which ordered locks a thread takes next. An ordered lock
 * must never be taken recursively.
 */

#ifndef OSSL_INTERNAL_RECORD_REPLAY_H
#define OSSL_INTERNAL_RECORD_REPLAY_H
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <openssl/e_os2.h>

#if defined(OPENSSL_THREADS) && !defined(CRYPTO_TDEBUG) \
    && !defined(OPENSSL_SYS_WINDOWS) && defined(__GNUC__)
#define OSSL_RECORD_REPLAY

extern int V8RecordReplayIsRecording(void) __attribute__((weak));
extern int V8RecordReplayIsReplaying(void) __attribute__((weak));
extern size_t V8RecordReplayCreateOrderedLock(const char *name)
    __attribute__((weak));
extern void V8RecordReplayOrderedLock(int lock) __attribute__((weak));
extern void V8RecordReplayOrderedUnlock(int lock) __attribute__((weak));
extern uintptr_t V8RecordReplayValue(const char *why, uintptr_t value)
    __attribute__((weak));

static ossl_inline int ossl_record_replay_active(void)
{
    return V8RecordReplayIsRecording != NULL
        && (V8RecordReplayIsRecording() || V8RecordReplayIsReplaying());
}

static ossl_inline uintptr_t ossl_record_replay_value(const char *why,
                                                      uintptr_t value)
{
    return V8RecordReplayValue != NULL ? V8RecordReplayValue(why, value)
                                       : value;
}

/*
 * One ordered lock for all tsan_assist.h operations. Created by
 * ossl_init_base, i.e. on the thread which initializes OpenSSL before any
 * other thread uses it; until then the operations are unordered.
 */
void ossl_record_replay_tsan_init(void);
int ossl_record_replay_tsan_lock(void);
void ossl_record_replay_tsan_unlock(int lock);

#define OSSL_RECORD_REPLAY_TSAN(expr)                        \
    __extension__({                                          \
        int ossl_rr_lock_ = ossl_record_replay_tsan_lock();  \
        __typeof__(expr) ossl_rr_ret_ = (expr);              \
        ossl_record_replay_tsan_unlock(ossl_rr_lock_);       \
        ossl_rr_ret_;                                        \
    })
#define OSSL_RECORD_REPLAY_TSAN_VOID(expr)                   \
    __extension__({                                          \
        int ossl_rr_lock_ = ossl_record_replay_tsan_lock();  \
        (expr);                                              \
        ossl_record_replay_tsan_unlock(ossl_rr_lock_);       \
    })

#else /* OSSL_RECORD_REPLAY */

#define ossl_record_replay_active() 0
#define ossl_record_replay_value(why, value) (value)
#define ossl_record_replay_tsan_init() ((void)0)

#endif /* OSSL_RECORD_REPLAY */

#endif
