#ifndef V8_REPLAY_WEAK_REFS_H_
#define V8_REPLAY_WEAK_REFS_H_

#include "src/objects/objects.h"

namespace v8 {
namespace internal {
class Isolate;
}  // namespace internal

namespace replayio {

class ReplayWeakRefPins {
 public:
  static void Pin(internal::Isolate* isolate, internal::HeapObject target);
  static void Unpin(internal::Isolate* isolate, internal::HeapObject target);
};

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_WEAK_REFS_H_
