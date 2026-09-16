#include "tth/TurnReportGate.h"

namespace tth {

TurnReportGate::TurnReportGate()
    : _turnActive(false),
      _stopRequested(false),
      _summaryPending(false),
      _wasBusy(false) {}

void TurnReportGate::onStart() {
  _turnActive = true;
  _stopRequested = false;
  _summaryPending = false;
  _wasBusy = true;
}

void TurnReportGate::onStopRequested() {
  if (!_turnActive) return;
  _stopRequested = true;
  // Armed, but not due until the hardware is actually released.
  _summaryPending = true;
}

bool TurnReportGate::shouldPrintStatus() const {
  return _turnActive && !_stopRequested;
}

bool TurnReportGate::takeSummaryDue(bool captureBusy) {
  const bool wasBusy = _wasBusy;
  _wasBusy = captureBusy;

  if (!_summaryPending) return false;
  // The summary is due on the falling edge of "still owns hardware". Before
  // that the bus is still held and a summary would misreport it.
  if (captureBusy) return false;
  if (!wasBusy && !captureBusy && !_turnActive) return false;

  _summaryPending = false;
  _turnActive = false;
  _stopRequested = false;
  return true;
}

}  // namespace tth
