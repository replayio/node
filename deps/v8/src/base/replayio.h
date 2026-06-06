// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file is a clone of "base/optional.h" in chromium.
// Keep in sync, especially when fixing bugs.
// Copyright 2017 the V8 project authors. All rights reserved.

#ifndef V8_BASE_REPLAYIO_H
#define V8_BASE_REPLAYIO_H

#include "include/replayio.h"

#include "src/base/optional.h"

namespace v8 {
namespace replayio {

struct AutoMaybeDisallowEvents {
  AutoMaybeDisallowEvents(bool disallowEvents, const char* label) {
    if (disallowEvents) {
      disallow.emplace(label);
    }
  }
  
private:
  v8::base::Optional<v8::replayio::AutoDisallowEvents> disallow;
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_BASE_REPLAYIO_H
