#pragma once

#include <stdint.h>

// Attributes drops of the internal heap's historical low-water mark
// (Step 6.2 memory investigation).
//
// The heap keeps one monotonic "minimum free since boot" figure. The heartbeat
// shows it every 10 s, which says THAT a large transient allocation happened
// but not WHERE. The loop reads the figure at a few checkpoints; when it has
// fallen by at least `reportStepBytes` since the last report, this tracker says
// so and names the checkpoint that first saw it. Work done synchronously in
// the loop is attributed to the stage just before its checkpoint; work done by
// other tasks (Wi-Fi, TLS, I2S) shows up at the first checkpoint after they
// ran, which the device labels accordingly.
//
// It only observes. A drop is not a leak: the current free figure recovering
// afterwards is what distinguishes a transient from a leak.
//
// Portable: the caller supplies the figure.

namespace tth {

class LowWaterTracker {
 public:
  explicit LowWaterTracker(uint32_t reportStepBytes);

  // Starts from the current low-water mark without reporting it.
  void reset(uint32_t lowWater);

  // True when `lowWater` is at least reportStepBytes below the last reported
  // (or reset) figure; stage() and lastDrop() then describe it.
  bool observe(uint32_t lowWater, const char* stage);

  bool started() const { return _started; }
  uint32_t lowWater() const { return _low; }
  uint32_t reportedLowWater() const { return _reported; }
  uint32_t lastDrop() const { return _lastDrop; }
  const char* stage() const { return _stage; }
  uint32_t reports() const { return _reports; }
  // Every observed decrease, reported or not.
  uint32_t decreases() const { return _decreases; }

 private:
  const uint32_t _step;
  bool _started;
  uint32_t _low;
  uint32_t _reported;
  uint32_t _lastDrop;
  const char* _stage;
  uint32_t _reports;
  uint32_t _decreases;
};

}  // namespace tth
