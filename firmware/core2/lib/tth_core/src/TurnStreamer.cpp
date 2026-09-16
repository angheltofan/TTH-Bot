#include "tth/TurnStreamer.h"

namespace tth {

const char* toString(StreamStatus status) {
  switch (status) {
    case StreamStatus::Idle:
      return "idle";
    case StreamStatus::Streaming:
      return "streaming";
    case StreamStatus::Finished:
      return "finished";
    case StreamStatus::Failed:
      return "failed";
  }
  return "invalid";
}

TurnStreamer::TurnStreamer(uint32_t frameSamples, uint32_t maxFramesPerService)
    : _frameSamples(frameSamples == 0 ? 1 : frameSamples),
      _maxFramesPerService(maxFramesPerService == 0 ? 1 : maxFramesPerService),
      _buffer(nullptr),
      _sink(nullptr),
      _format(monoS16(0)),
      _status(StreamStatus::Idle),
      _cursor(0) {
  _metrics.streamedSamples = 0;
  _metrics.frames = 0;
  _metrics.busyRetries = 0;
  _metrics.maxLagSamples = 0;
}

bool TurnStreamer::begin(TurnBuffer& buffer, ITurnSource& sink,
                         const AudioFormat& format) {
  if (_status == StreamStatus::Streaming) return false;
  if (!buffer.retain()) return false;

  if (!sink.beginUserTurn(format)) {
    buffer.release();
    return false;
  }

  _buffer = &buffer;
  _sink = &sink;
  _format = format;
  _cursor = 0;
  _metrics.streamedSamples = 0;
  _metrics.frames = 0;
  _metrics.busyRetries = 0;
  _metrics.maxLagSamples = 0;
  _status = StreamStatus::Streaming;
  return true;
}

uint32_t TurnStreamer::lagSamples() const {
  if (_buffer == nullptr || _status != StreamStatus::Streaming) return 0;
  return _buffer->committedSamples() - _cursor;
}

StreamStatus TurnStreamer::service(bool producerDone) {
  if (_status != StreamStatus::Streaming) return _status;

  for (uint32_t sent = 0; sent < _maxFramesPerService; ++sent) {
    // The ONLY length read: committed samples, loaded with acquire ordering.
    const uint32_t committed = _buffer->committedSamples();
    const uint32_t available = committed - _cursor;
    if (available == 0) break;

    const uint32_t count =
        (available < _frameSamples) ? available : _frameSamples;
    // While the recording is live, a short remainder waits to become a whole
    // frame. Only the very last frame of a turn may be short.
    if (count < _frameSamples && !producerDone) break;

    AudioChunk chunk;
    chunk.samples = _buffer->data() + _cursor;
    chunk.count = count;
    chunk.format = _format;

    const PushResult result = _sink->pushUserAudio(chunk);
    if (result == PushResult::Busy) {
      // Cursor unchanged: exactly these samples are offered again next time.
      ++_metrics.busyRetries;
      break;
    }
    if (result == PushResult::Fatal) {
      releaseLease();
      _status = StreamStatus::Failed;
      return _status;
    }

    _cursor += count;
    _metrics.streamedSamples += count;
    ++_metrics.frames;
  }

  const uint32_t lag = _buffer->committedSamples() - _cursor;
  if (lag > _metrics.maxLagSamples) _metrics.maxLagSamples = lag;

  // Done only when the recording has stopped AND every committed sample has
  // been accepted. Until then the lease is kept, so the buffer cannot be
  // reset under the unsent tail.
  if (producerDone && lag == 0) {
    _sink->endUserTurn();
    releaseLease();
    _status = StreamStatus::Finished;
  }
  return _status;
}

void TurnStreamer::abandon() {
  if (_status != StreamStatus::Streaming) return;
  releaseLease();
  _status = StreamStatus::Idle;
}

void TurnStreamer::releaseLease() {
  if (_buffer != nullptr) _buffer->release();
  _buffer = nullptr;
}

}  // namespace tth
