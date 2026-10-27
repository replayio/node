// Copyright (c) 2024 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// API for interacting with the record/replay driver.
// Some parts are still in v8.h and still need to be migrated.


#ifndef INCLUDE_REPLAYIO_MACROS_H_
#define INCLUDE_REPLAYIO_MACROS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <string>
#include <type_traits>
#else
#include <stdbool.h>
#endif

#ifdef __cplusplus
// A detail of a structured warning (see recordreplay::Warning), handed to the
// driver's RecordReplayStructuredWarning. Same layout as the driver's
// RecordReplayWarningField.
struct RecordReplayWarningField {
  enum Type : int { kString, kInt, kUnsigned, kDouble, kBool };

  RecordReplayWarningField(const char* key, const char* value)
      : key(key), type(kString), string_value(value) {}
  // The string must outlive the warning call, which holds for temporaries in
  // the initializer list of a Warning(...) call.
  RecordReplayWarningField(const char* key, const std::string& value)
      : key(key), type(kString), string_value(value.c_str()) {}
  RecordReplayWarningField(const char* key, bool value)
      : key(key), type(kBool), bool_value(value) {}
  RecordReplayWarningField(const char* key, double value)
      : key(key), type(kDouble), double_value(value) {}
  template <typename T,
            typename std::enable_if<std::is_integral<T>::value &&
                                        !std::is_same<T, bool>::value,
                                    int>::type = 0>
  RecordReplayWarningField(const char* key, T value) : key(key) {
    if (std::is_signed<T>::value) {
      type = kInt;
      int_value = static_cast<int64_t>(value);
    } else {
      type = kUnsigned;
      unsigned_value = static_cast<uint64_t>(value);
    }
  }

  const char* key;
  Type type;
  union {
    const char* string_value;
    int64_t int_value;
    uint64_t unsigned_value;
    double double_value;
    bool bool_value;
  };
};
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool V8RecordReplayHasAsserts(void);
void V8RecordReplayAssert(const char* format, ...);
void V8RecordReplayAssertMaybeEventsDisallowed(const char* format, ...);
#ifdef __cplusplus
void V8RecordReplayStructuredWarning(const char* kind,
                                     const struct RecordReplayWarningField* fields,
                                     size_t count);
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

// Use this to wrap Asserts on non-trivial data, to avoid the
// overhead of argument evaluation when Asserts are disabled.
#define REPLAY_ASSERT(format, ...) \
  if (V8RecordReplayHasAsserts()) \
    V8RecordReplayAssert(format, ##__VA_ARGS__); \
  ((void)0)

// Same as |REPLAY_ASSERT| but won't Assert when Events are disallowed.
#define REPLAY_ASSERT_MAYBE_EVENTS_DISALLOWED(format, ...) \
  if (V8RecordReplayHasAsserts()) \
    V8RecordReplayAssertMaybeEventsDisallowed(format, ##__VA_ARGS__); \
  ((void)0)

#endif  // INCLUDE_REPLAYIO_MACROS_H_
