#include "diag/SerialLog.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "tth/Config.h"

namespace tth {
namespace diag {

namespace {

// The UART, as LogDrainer sees it. Nothing here blocks: availableForWrite()
// reports the free space and write() is given no more than that.
class ArduinoSerialSink : public ILogSink {
 public:
  size_t availableForWrite() override {
    const int free = Serial.availableForWrite();
    return free > 0 ? static_cast<size_t>(free) : 0u;
  }

  size_t write(const uint8_t* data, size_t length) override {
    return Serial.write(data, length);
  }
};

ArduinoSerialSink g_sink;

}  // namespace

SerialLog::SerialLog()
    : _drainer(_queue, TTH_LOG_WRITE_BUDGET), _maxWriteMicros(0) {}

void SerialLog::printf(const char* format, ...) {
  char line[kLogMessageMax];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  _queue.push(line);
}

uint32_t SerialLog::service() {
  const uint32_t start = micros();
  _drainer.service(g_sink);
  const uint32_t elapsed = micros() - start;

  // Per partial write, which is what bounds a loop iteration. The time to get
  // a whole line onto the wire is spread across many of these and is not a
  // blocking cost.
  if (elapsed > _maxWriteMicros) _maxWriteMicros = elapsed;
  return elapsed;
}

SerialLog& log() {
  static SerialLog instance;
  return instance;
}

}  // namespace diag
}  // namespace tth
