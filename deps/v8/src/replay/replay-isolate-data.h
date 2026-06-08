#ifndef V8_REPLAY_REPLAY_ISOLATE_DATA_H_
#define V8_REPLAY_REPLAY_ISOLATE_DATA_H_

#include <vector>

#include "include/v8-persistent-handle.h"

namespace v8 {
namespace replayio {

// General-purpose per-Isolate data for recording and replaying.
class ReplayIsolateData {
 public:
  ReplayIsolateData() = default;
  ~ReplayIsolateData() = default;

  ReplayIsolateData(const ReplayIsolateData&) = delete;
  ReplayIsolateData& operator=(const ReplayIsolateData&) = delete;

  std::vector<v8::Global<v8::Value>>& weak_ref_pins() { return weak_ref_pins_; }

 private:
  std::vector<v8::Global<v8::Value>> weak_ref_pins_;
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_REPLAY_ISOLATE_DATA_H_
