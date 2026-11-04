#ifndef V8_REPLAY_REPLAYIO_H_
#define V8_REPLAY_REPLAYIO_H_

namespace v8 {
namespace replayio {

// Whether the current point replays, so that values can be recorded/replayed
// and ids handed out consistently.
bool AreEventsAvailable();

}  // namespace replayio
}  // namespace v8

#endif  // V8_REPLAY_REPLAYIO_H_
