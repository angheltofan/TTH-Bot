#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "tth/DeviceConfig.h"
#include "tth/DownstreamCredit.h"
#include "tth/DownstreamRing.h"
#include "tth/GatewayProtocol.h"
#include "tth/GatewaySession.h"
#include "tth/IUplink.h"
#include "tth/OutboundQueue.h"
#include "tth/WebSocketCodec.h"

namespace tth {

class VerifiedTlsClient;

// The TLS WebSocket transport to the gateway (Steps 6.2, 6.3).
//
// ONE FreeRTOS task (core 0) owns the socket entirely: DNS, TCP, the TLS
// handshake, the upgrade, every read and every write. Blocking TLS therefore
// never touches the cooperative loop on core 1.
//
// WHO PRODUCES AND WHO CONSUMES (Step 6.3)
//
//   loop -> task   commands (connect / close, tagged with a generation);
//                  hello and ping text frames (a FreeRTOS queue);
//                  turn frames and credit: the ONE ordered OutboundQueue,
//                  shared under a mutex. The loop pushes; the task (the
//                  NetSender) takes the item at a frame boundary, writes it
//                  with the lock RELEASED, and removes it only after the whole
//                  frame was written. A failed or partial write fails the send
//                  and drops the connection.
//   task -> loop   events (connected, connect failed, text, closed, violation),
//                  tagged with the connection generation;
//                  model audio: the DownstreamRing (SPSC, lock-free; a frame
//                  becomes visible only when completely written);
//                  lastTurnEndSent(): the turn whose turn_end was completely
//                  written.
//
// Critical sections hold the queue mutex only for a queue operation -- never
// for I/O -- so they are bounded.
//
// THE TASK NEVER WAITS FOR CREDIT OR SPACE. A model frame beyond the credit
// or the ring is a protocol violation: nothing already in the ring is
// touched, the violation is counted and reported, and the connection is
// closed so the robot reconnects with clean accounting.
//
// The task NEVER logs (the log queue is single-threaded) and never calls App.
// A control event that cannot be queued would lose protocol state, so it
// closes the connection instead of being silently dropped. Every connect and
// every close ends in exactly one terminal event (ConnectFailed or Closed).
//
// SECURITY
//   * The gateway certificate is verified against the provisioned CA and the
//     hostname (mbedTLS VERIFY_REQUIRED + set_hostname). The verification
//     result is checked again after the handshake. There is no insecure mode:
//     without a CA the task does not connect.
//   * Certificate validity DATES are not checked by this mbedTLS build
//     (PHASE6_PLAN §4.2 -- a documented residual gap).
//   * The upgrade request carries the device token; its buffer is zeroed as
//     soon as it has been written. The token is never logged.
//   * Audio is never logged, by the task or by anything reading its stats.
class GatewayClient : public IUplink {
 public:
  enum class EventType : uint8_t {
    None = 0,
    Connected,
    ConnectFailed,
    Text,
    Closed,
    ProtocolError,
    DownstreamViolation,
  };
  enum class CloseCause : uint8_t { Local = 0, Peer, Transport, WriteFailed, Protocol };

  // DownstreamViolation details.
  static const int32_t kViolationCredit = 1;
  static const int32_t kViolationCapacity = 2;

  struct Event {
    EventType type;
    CloseCause cause;
    ConnectFailure failure;
    uint32_t generation;
    uint32_t turn;         // DownstreamViolation: the frame's turn
    int32_t detail;        // HTTP status, TLS error, close code, DecodeError, violation
    uint32_t elapsedMs;    // connected/failed: since the attempt began; closed: age
    uint32_t heapFree;     // internal heap after the TLS handshake (connected)
    uint32_t heapLargest;
    // Internal heap low-water mark when the attempt began and when it ended:
    // a drop between them happened during DNS, TCP, TLS or the upgrade.
    uint32_t lowWaterBefore;
    uint32_t lowWaterAfter;
    uint16_t length;
    char text[wire::kMaxControlBytes + 1];
  };

  struct Stats {
    uint32_t eventDrops;
    uint32_t pingsAnswered;
    uint32_t tlsAllocFailures;
    uint32_t maxHandshakeMs;
    uint32_t maxWriteMs;
    uint32_t bytesIn;
    uint32_t bytesOut;
    uint32_t stackHighWater;
    // Step 6.3
    uint32_t itemsSent;
    uint32_t sendFailures;
    uint32_t downFrames;
    uint32_t downBytes;
    uint32_t badFrames;
    uint32_t oldConnectionFrames;
    uint32_t capacityViolations;
    uint32_t zeroCreditStalls;  // frames that left the gateway exactly 0 credit
  };

  GatewayClient();

  // Allocates the buffers (PSRAM, no fallback), the queues and the task.
  bool begin();

  // Snapshot of where to connect. Taken by the task at the next connect.
  void setTarget(const config::DeviceConfig& config, const char* caPem, size_t caLength);
  void clearTarget();

  bool connect(uint32_t generation);
  bool close(uint32_t generation);

  // Queues one text frame (<= 512 B): hello and ping. False when full.
  bool sendText(const char* text, size_t length);

  bool nextEvent(Event& out);

  // --- IUplink (loop) ---
  OutPush pushTurnStart(uint32_t turn) override;
  OutPush pushAudio(uint32_t turn, const int16_t* pcm, uint32_t samples) override;
  OutPush pushTurnEnd(uint32_t turn, uint32_t frames, uint32_t bytes) override;
  void requestCancel(uint32_t turn) override;
  OutPush pushCredit(uint32_t bytes) override;
  uint32_t lastTurnEndSent() const override;

  // The downstream ring and its credit. The loop is their only consumer.
  DownstreamRing& ring() { return _ring; }
  DownstreamCredit& credit() { return _credit; }

  Stats stats() const { return _stats; }
  uint32_t outboundDrops() const { return _outboundDrops; }
  uint32_t creditViolations() const;
  uint32_t outboundHighWater() const;
  OutboundCounters outboundCounters() const;
  const void* ringStorage() const { return _ringStorage; }
  const void* queueStorage() const { return _queue; }

 private:
  struct Target {
    bool valid;
    config::GatewayUrlParts url;
    char deviceId[config::kDeviceIdMaxBytes + 1];
    char token[config::kTokenBytes + 1];
    size_t caLength;
  };

  static void taskEntry(void* self);
  void run();
  void doConnect();
  void serviceOpen();
  bool sendQueuedItems();
  void handleFrame(const ws::Frame& frame);
  void handleModelAudio(const ws::Frame& frame);
  void violation(uint32_t turn, int32_t kind);
  bool writeFrame(ws::Opcode opcode, const uint8_t* payload, size_t length);
  void closeWith(uint16_t code, CloseCause cause);
  void drop(CloseCause cause);
  void postConnectFailed(ConnectFailure failure, int32_t detail, uint32_t startedMs);
  void postClosed(uint32_t generation, CloseCause cause, int32_t code);
  bool post(bool terminal);

  TaskHandle_t _task;
  QueueHandle_t _commands;
  QueueHandle_t _outbound;
  QueueHandle_t _events;
  SemaphoreHandle_t _targetMutex;

  Target _pendingTarget;  // guarded by _targetMutex
  char* _pendingCa;       // guarded by _targetMutex

  // Shared with the loop.
  SemaphoreHandle_t _queueMutex;
  OutboundQueue* _queue;  // PSRAM; guarded by _queueMutex
  uint8_t* _ringStorage;  // PSRAM
  DownstreamRing _ring;
  DownstreamCredit _credit;
  std::atomic<uint32_t> _turnEndSent;

  // Owned by the task.
  VerifiedTlsClient* _tls;
  Target _target;
  char* _caCopy;
  uint8_t* _rx;
  uint8_t* _frameBuffer;
  uint8_t* _tx;
  char* _request;
  char* _head;
  ws::FrameDecoder _decoder;
  bool _open;
  uint32_t _generation;
  uint32_t _openedAtMs;
  uint32_t _connectLowWater;
  Event _event;
  Stats _stats;

  // Owned by the loop.
  uint32_t _outboundDrops;
};

}  // namespace tth
