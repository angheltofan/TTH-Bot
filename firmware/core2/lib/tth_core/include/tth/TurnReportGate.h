#pragma once

#include <stdint.h>

// Decides WHEN a turn may report, so the log describes reality.
//
// THE DEFECT THIS FIXES
//
// The summary was printed the instant the button was released, which is before
// the microphone has drained and before AudioBus has released. So it claimed
// `audio=mic`, and was followed by `[mic] stopped` -- the summary described a
// state that had not happened yet. Worse, a routine status line could still be
// emitted afterwards, so a forced stop logged a 45000 ms status line after its
// own summary.
//
// The rule: a stop request gets a short acknowledgement immediately; the
// SUMMARY waits until the hardware is genuinely released, and no routine
// status may be printed once a stop has been requested.
//
// Portable, so the ordering is unit tested rather than eyeballed in a log.

namespace tth {

class TurnReportGate {
 public:
  TurnReportGate();

  // A turn began. Re-arms status output.
  void onStart();

  // The turn is ending. Status output stops here, even though the hardware is
  // still draining.
  void onStopRequested();

  // Routine per-interval status is allowed only while a turn is running and no
  // stop has been requested.
  bool shouldPrintStatus() const;

  // Call every loop with whether the capture controller still owns anything.
  // Returns true EXACTLY ONCE per turn: on the transition to fully released,
  // which is the only moment a summary can honestly say `audio=none`.
  bool takeSummaryDue(bool captureBusy);

  bool isStopRequested() const { return _stopRequested; }

 private:
  bool _turnActive;
  bool _stopRequested;
  bool _summaryPending;
  bool _wasBusy;
};

}  // namespace tth
