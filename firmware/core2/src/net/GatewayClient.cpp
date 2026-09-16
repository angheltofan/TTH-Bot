#include "net/GatewayClient.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <soc/soc_memory_layout.h>

#include <cstring>
#include <new>

#include "tth/Config.h"
#include "tth/TrustAnchor.h"

namespace tth {

// WiFiClientSecure with the one thing its API hides: the verification result.
// The handshake already fails unless the chain verifies against the pinned CA
// for the right host name (VERIFY_REQUIRED); this reads the result again, as
// an independent second check.
class VerifiedTlsClient : public WiFiClientSecure {
 public:
  bool verified() {
    return sslclient != nullptr &&
           mbedtls_ssl_get_verify_result(&sslclient->ssl_ctx) == 0;
  }
};

namespace {

const uint8_t kCommandConnect = 1;
const uint8_t kCommandClose = 2;

const size_t kRxChunk = 1024;
// The largest server frame tth.v1 allows is 8 + 1920 bytes of model audio.
const size_t kFrameBufferBytes = 2048;
const size_t kTxBufferBytes = ws::kMaxClientFrameHeader + kFrameBufferBytes;

// ConnectFailed / ProtocolError details that are not an HTTP status, a TLS
// error code or a DecodeError.
const int32_t kDetailNoTarget = -3;
const int32_t kDetailRequest = -4;
const int32_t kDetailUpgradeTimeout = -5;
const int32_t kDetailReadFailed = -6;
const int32_t kDetailNotVerified = -7;
const int32_t kDetailOversizeText = -8;
const int32_t kDetailBadAudioFrame = -9;

struct Command {
  uint8_t type;
  uint32_t generation;
};

struct Outbound {
  uint16_t length;
  char data[wire::kMaxControlBytes];
};

bool isAllocationError(int error) {
  return error == MBEDTLS_ERR_SSL_ALLOC_FAILED || error == MBEDTLS_ERR_X509_ALLOC_FAILED ||
         error == MBEDTLS_ERR_PK_ALLOC_FAILED || error == MBEDTLS_ERR_ECP_ALLOC_FAILED ||
         error == MBEDTLS_ERR_MPI_ALLOC_FAILED;
}

uint32_t internalFree() {
  return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

uint32_t internalLowWater() {
  return static_cast<uint32_t>(
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

uint32_t internalLargest() {
  return static_cast<uint32_t>(
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void* psramCalloc(size_t bytes) { return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM); }

// Holds the outbound-queue mutex for one queue operation. Never across I/O.
class QueueLock {
 public:
  explicit QueueLock(SemaphoreHandle_t mutex) : _mutex(mutex) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
  }
  ~QueueLock() { xSemaphoreGive(_mutex); }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;

 private:
  SemaphoreHandle_t _mutex;
};

}  // namespace

GatewayClient::GatewayClient()
    : _task(nullptr),
      _commands(nullptr),
      _outbound(nullptr),
      _events(nullptr),
      _targetMutex(nullptr),
      _pendingCa(nullptr),
      _queueMutex(nullptr),
      _queue(nullptr),
      _ringStorage(nullptr),
      _credit(TTH_GW_INITIAL_CREDIT, TTH_GW_CREDIT_BATCH_BYTES),
      _turnEndSent(0),
      _tls(nullptr),
      _caCopy(nullptr),
      _rx(nullptr),
      _frameBuffer(nullptr),
      _tx(nullptr),
      _request(nullptr),
      _head(nullptr),
      _decoder(nullptr, 0),
      _open(false),
      _generation(0),
      _openedAtMs(0),
      _connectLowWater(0),
      _outboundDrops(0) {
  memset(&_pendingTarget, 0, sizeof(_pendingTarget));
  memset(&_target, 0, sizeof(_target));
  memset(&_event, 0, sizeof(_event));
  memset(&_stats, 0, sizeof(_stats));
}

bool GatewayClient::begin() {
  if (_task != nullptr) return true;

  // PSRAM: none of these is touched by DMA or an ISR, only by our task.
  _pendingCa = static_cast<char*>(psramCalloc(trust::kMaxPemBytes + 1));
  _caCopy = static_cast<char*>(psramCalloc(trust::kMaxPemBytes + 1));
  _rx = static_cast<uint8_t*>(psramCalloc(kRxChunk));
  _frameBuffer = static_cast<uint8_t*>(psramCalloc(kFrameBufferBytes));
  _tx = static_cast<uint8_t*>(psramCalloc(kTxBufferBytes));
  _request = static_cast<char*>(psramCalloc(ws::kMaxRequestBytes));
  _head = static_cast<char*>(psramCalloc(ws::kMaxResponseHeadBytes + 1));
  if (_pendingCa == nullptr || _caCopy == nullptr || _rx == nullptr ||
      _frameBuffer == nullptr || _tx == nullptr || _request == nullptr || _head == nullptr) {
    return false;
  }
  _decoder = ws::FrameDecoder(_frameBuffer, kFrameBufferBytes);

  // Step 6.3: the downstream ring and the outbound queue, in PSRAM ONLY.
  // MALLOC_CAP_SPIRAM never falls back to internal RAM; the region is checked
  // anyway, and a failure disables the gateway rather than moving them.
  _ringStorage = static_cast<uint8_t*>(psramCalloc(TTH_GW_RING_BYTES));
  void* queueMemory = heap_caps_malloc(sizeof(OutboundQueue), MALLOC_CAP_SPIRAM);
  if (_ringStorage == nullptr || queueMemory == nullptr ||
      !esp_ptr_external_ram(_ringStorage) || !esp_ptr_external_ram(queueMemory)) {
    return false;
  }
  _queue = new (queueMemory) OutboundQueue();
  if (!_ring.attach(_ringStorage, TTH_GW_RING_BYTES)) return false;

  _commands = xQueueCreate(4, sizeof(Command));
  _outbound = xQueueCreate(TTH_GW_OUTBOUND_QUEUE_LENGTH, sizeof(Outbound));
  _events = xQueueCreate(TTH_GW_EVENT_QUEUE_LENGTH, sizeof(Event));
  _targetMutex = xSemaphoreCreateMutex();
  _queueMutex = xSemaphoreCreateMutex();
  if (_commands == nullptr || _outbound == nullptr || _events == nullptr ||
      _targetMutex == nullptr || _queueMutex == nullptr) {
    return false;
  }
  return xTaskCreatePinnedToCore(&GatewayClient::taskEntry, "tth-gateway",
                                 TTH_GW_TASK_STACK_BYTES, this, TTH_GW_TASK_PRIORITY,
                                 &_task, TTH_GW_TASK_CORE) == pdPASS;
}

void GatewayClient::setTarget(const config::DeviceConfig& c, const char* caPem,
                              size_t caLength) {
  if (_targetMutex == nullptr) return;
  Target target;
  memset(&target, 0, sizeof(target));
  if (config::checkGatewayUrl(c.gatewayUrl, c.gatewayUrlLength, &target.url) ==
          config::Check::Ok &&
      caPem != nullptr && caLength > 0 && caLength <= trust::kMaxPemBytes) {
    memcpy(target.deviceId, c.deviceId, c.deviceIdLength);
    memcpy(target.token, c.deviceToken, c.deviceTokenLength);
    target.caLength = caLength;
    target.valid = true;
  }
  // The task holds the mutex only for two memcpy calls.
  xSemaphoreTake(_targetMutex, portMAX_DELAY);
  _pendingTarget = target;
  if (target.valid) {
    memcpy(_pendingCa, caPem, caLength);
    _pendingCa[caLength] = '\0';
  }
  xSemaphoreGive(_targetMutex);
  memset(&target, 0, sizeof(target));
}

void GatewayClient::clearTarget() {
  if (_targetMutex == nullptr) return;
  xSemaphoreTake(_targetMutex, portMAX_DELAY);
  memset(&_pendingTarget, 0, sizeof(_pendingTarget));
  xSemaphoreGive(_targetMutex);
}

bool GatewayClient::connect(uint32_t generation) {
  if (_commands == nullptr) return false;
  Command command = {kCommandConnect, generation};
  return xQueueSend(_commands, &command, 0) == pdTRUE;
}

bool GatewayClient::close(uint32_t generation) {
  if (_commands == nullptr) return false;
  Command command = {kCommandClose, generation};
  return xQueueSend(_commands, &command, 0) == pdTRUE;
}

bool GatewayClient::sendText(const char* text, size_t length) {
  if (_outbound == nullptr || text == nullptr || length == 0 ||
      length > wire::kMaxControlBytes) {
    return false;
  }
  Outbound item;
  item.length = static_cast<uint16_t>(length);
  memcpy(item.data, text, length);
  if (xQueueSend(_outbound, &item, 0) != pdTRUE) {
    ++_outboundDrops;
    return false;
  }
  return true;
}

bool GatewayClient::nextEvent(Event& out) {
  return _events != nullptr && xQueueReceive(_events, &out, 0) == pdTRUE;
}

// --- IUplink (loop) --------------------------------------------------------------------

OutPush GatewayClient::pushTurnStart(uint32_t turn) {
  if (_queue == nullptr) return OutPush::Busy;
  QueueLock lock(_queueMutex);
  return _queue->pushTurnStart(turn);
}

OutPush GatewayClient::pushAudio(uint32_t turn, const int16_t* pcm, uint32_t samples) {
  if (_queue == nullptr) return OutPush::Busy;
  QueueLock lock(_queueMutex);
  // The whole frame is copied into its slot before the lock is released, so
  // the sender can never see a partly written item.
  return _queue->pushAudio(turn, pcm, samples);
}

OutPush GatewayClient::pushTurnEnd(uint32_t turn, uint32_t frames, uint32_t bytes) {
  if (_queue == nullptr) return OutPush::Busy;
  QueueLock lock(_queueMutex);
  return _queue->pushTurnEnd(turn, frames, bytes);
}

void GatewayClient::requestCancel(uint32_t turn) {
  if (_queue == nullptr) return;
  QueueLock lock(_queueMutex);
  _queue->requestCancel(turn);
}

OutPush GatewayClient::pushCredit(uint32_t bytes) {
  if (_queue == nullptr) return OutPush::Busy;
  QueueLock lock(_queueMutex);
  return _queue->pushCredit(bytes);
}

uint32_t GatewayClient::lastTurnEndSent() const {
  return _turnEndSent.load(std::memory_order_acquire);
}

uint32_t GatewayClient::creditViolations() const {
  return _credit.violations() + _stats.capacityViolations;
}

uint32_t GatewayClient::outboundHighWater() const {
  if (_queue == nullptr) return 0;
  QueueLock lock(_queueMutex);
  return _queue->highWater();
}

OutboundCounters GatewayClient::outboundCounters() const {
  OutboundCounters counters;
  memset(&counters, 0, sizeof(counters));
  if (_queue == nullptr) return counters;
  QueueLock lock(_queueMutex);
  counters = _queue->counters();
  return counters;
}

// --- the task ------------------------------------------------------------------------

void GatewayClient::taskEntry(void* self) { static_cast<GatewayClient*>(self)->run(); }

void GatewayClient::run() {
  _tls = new VerifiedTlsClient();
  Command command;
  for (;;) {
    const TickType_t wait = _open ? 0 : pdMS_TO_TICKS(50);
    if (xQueueReceive(_commands, &command, wait) == pdTRUE) {
      if (command.type == kCommandConnect) {
        if (_open) drop(CloseCause::Local);
        _generation = command.generation;
        doConnect();
      } else if (_open && command.generation == _generation) {
        closeWith(1000, CloseCause::Local);
      } else {
        // Nothing open for that generation: still end the close.
        postClosed(command.generation, CloseCause::Local, 1000);
      }
      _stats.stackHighWater = uxTaskGetStackHighWaterMark(nullptr);
      continue;
    }
    if (_open) serviceOpen();
  }
}

void GatewayClient::doConnect() {
  const uint32_t startedMs = millis();
  _connectLowWater = internalLowWater();
  _openedAtMs = 0;
  xQueueReset(_outbound);  // nothing from an older session may be sent
  _decoder.reset();
  // A new connection starts with a clean outbound path and a fresh credit
  // epoch on this side. (The loop restarted its own epoch and emptied the
  // ring BEFORE it queued this connect, while nothing was being written.)
  {
    QueueLock lock(_queueMutex);
    _queue->reset();
  }
  _credit.beginProducerEpoch();

  bool haveTarget = false;
  if (xSemaphoreTake(_targetMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    _target = _pendingTarget;
    if (_target.valid) {
      memcpy(_caCopy, _pendingCa, _target.caLength);
      _caCopy[_target.caLength] = '\0';
    }
    haveTarget = _target.valid;
    xSemaphoreGive(_targetMutex);
  }
  if (!haveTarget) {
    postConnectFailed(ConnectFailure::Network, kDetailNoTarget, startedMs);
    return;
  }

  IPAddress address;
  if (!WiFi.hostByName(_target.url.host, address)) {
    postConnectFailed(ConnectFailure::Dns, 0, startedMs);
    return;
  }

  // Pinned CA only; never setInsecure().
  _tls->setCACert(_caCopy);
  _tls->setHandshakeTimeout(TTH_GW_TLS_HANDSHAKE_S);
  _tls->setTimeout(TTH_GW_SOCKET_TIMEOUT_S);
  if (!_tls->connect(address, _target.url.port, _target.url.host, _caCopy, nullptr,
                     nullptr)) {
    char text[8];
    const int error = _tls->lastError(text, sizeof(text));
    ConnectFailure failure = ConnectFailure::Network;
    if (error < 0 && error != -1) {
      failure = isAllocationError(error) ? ConnectFailure::TlsAlloc : ConnectFailure::Tls;
    }
    if (failure == ConnectFailure::TlsAlloc) ++_stats.tlsAllocFailures;
    _tls->stop();
    postConnectFailed(failure, error, startedMs);
    return;
  }
  if (!_tls->verified()) {
    _tls->stop();
    postConnectFailed(ConnectFailure::Tls, kDetailNotVerified, startedMs);
    return;
  }

  // --- the WebSocket upgrade ---------------------------------------------------
  uint8_t random16[16];
  esp_fill_random(random16, sizeof(random16));
  char key[ws::kKeyBase64Bytes + 1];
  char accept[ws::kAcceptBytes + 1];
  if (!ws::makeKey(random16, key) || !ws::computeAccept(key, accept)) {
    _tls->stop();
    postConnectFailed(ConnectFailure::Network, kDetailRequest, startedMs);
    return;
  }

  ws::UpgradeRequest request;
  request.host = _target.url.host;
  request.port = _target.url.port;
  request.path = _target.url.path;
  request.deviceId = _target.deviceId;
  request.token = _target.token;
  request.key = key;
  request.userAgent = "tth-" TTH_FIRMWARE_VERSION;
  const size_t length = ws::buildUpgradeRequest(request, _request, ws::kMaxRequestBytes);
  bool written = length > 0;
  size_t sent = 0;
  while (written && sent < length) {
    const size_t n =
        _tls->write(reinterpret_cast<const uint8_t*>(_request) + sent, length - sent);
    if (n == 0) {
      written = false;
    } else {
      sent += n;
    }
  }
  // The request carried the device token.
  memset(_request, 0, ws::kMaxRequestBytes);
  if (!written) {
    _tls->stop();
    postConnectFailed(ConnectFailure::Network, kDetailRequest, startedMs);
    return;
  }

  // Byte by byte, so no frame after the response head is consumed here.
  size_t fill = 0;
  const uint32_t deadline = millis() + TTH_GW_UPGRADE_TIMEOUT_MS;
  ws::HandshakeResult result;
  result.outcome = ws::HandshakeOutcome::Incomplete;
  result.status = 0;
  result.headBytes = 0;
  while (result.outcome == ws::HandshakeOutcome::Incomplete) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      _tls->stop();
      postConnectFailed(ConnectFailure::Network, kDetailUpgradeTimeout, startedMs);
      return;
    }
    const int available = _tls->available();
    if (available < 0) {
      _tls->stop();
      postConnectFailed(ConnectFailure::Network, kDetailReadFailed, startedMs);
      return;
    }
    if (available == 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    uint8_t byte = 0;
    if (_tls->read(&byte, 1) != 1) {
      _tls->stop();
      postConnectFailed(ConnectFailure::Network, kDetailReadFailed, startedMs);
      return;
    }
    _head[fill++] = static_cast<char>(byte);
    if (byte == '\n' || fill >= ws::kMaxResponseHeadBytes) {
      result = ws::parseUpgradeResponse(_head, fill, accept, ws::kSubprotocol);
    }
  }

  if (result.outcome == ws::HandshakeOutcome::Accepted) {
    _open = true;
    _openedAtMs = millis();
    const uint32_t elapsed = _openedAtMs - startedMs;
    if (elapsed > _stats.maxHandshakeMs) _stats.maxHandshakeMs = elapsed;
    memset(&_event, 0, sizeof(_event));
    _event.type = EventType::Connected;
    _event.generation = _generation;
    _event.elapsedMs = elapsed;
    _event.heapFree = internalFree();
    _event.heapLargest = internalLargest();
    _event.lowWaterBefore = _connectLowWater;
    _event.lowWaterAfter = internalLowWater();
    post(true);
    return;
  }

  _tls->stop();
  if (result.outcome == ws::HandshakeOutcome::HttpStatus) {
    postConnectFailed(failureForHttpStatus(result.status), result.status, startedMs);
  } else {
    postConnectFailed(ConnectFailure::BadHandshake, static_cast<int32_t>(result.outcome),
                      startedMs);
  }
}

void GatewayClient::serviceOpen() {
  // hello and ping first: hello must precede everything on a new connection,
  // and the loop queues no turn frame before the session is ready.
  Outbound item;
  while (_open && xQueueReceive(_outbound, &item, 0) == pdTRUE) {
    if (!writeFrame(ws::Opcode::Text, reinterpret_cast<const uint8_t*>(item.data),
                    item.length)) {
      drop(CloseCause::WriteFailed);
      return;
    }
  }
  if (!_open) return;
  if (!sendQueuedItems()) return;

  const int available = _tls->available();
  if (available < 0) {
    drop(CloseCause::Transport);
    return;
  }
  if (available == 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }
  const size_t want = static_cast<size_t>(available) > kRxChunk ? kRxChunk
                                                                : static_cast<size_t>(available);
  const int n = _tls->read(_rx, want);
  if (n <= 0) {
    drop(CloseCause::Transport);
    return;
  }
  _stats.bytesIn += static_cast<uint32_t>(n);

  size_t offset = 0;
  while (_open && offset < static_cast<size_t>(n)) {
    size_t used = 0;
    const ws::DecodeStatus status =
        _decoder.feed(_rx + offset, static_cast<size_t>(n) - offset, used);
    offset += used;
    if (status == ws::DecodeStatus::Frame) {
      handleFrame(_decoder.frame());
    } else if (status == ws::DecodeStatus::Error) {
      memset(&_event, 0, sizeof(_event));
      _event.type = EventType::ProtocolError;
      _event.generation = _generation;
      _event.detail = static_cast<int32_t>(_decoder.error());
      post(false);
      closeWith(1002, CloseCause::Protocol);
      return;
    } else if (used == 0) {
      break;
    }
  }
}

// The NetSender. An item stays in the queue while it is written, with the
// lock released; it is removed only once the whole frame is out. False if
// the connection was dropped.
bool GatewayClient::sendQueuedItems() {
  for (uint32_t i = 0; i < TTH_GW_SEND_ITEMS_PER_SERVICE; ++i) {
    const OutboundItem* item = nullptr;
    OutKind kind = OutKind::Ping;
    uint32_t turn = 0;
    {
      QueueLock lock(_queueMutex);
      // Pending cancels are applied here, at a frame boundary.
      item = _queue->beginSend();
      if (item != nullptr) {
        kind = item->kind;
        turn = item->turn;
      }
    }
    if (item == nullptr) return true;

    // The loop never writes an in-flight slot (OutboundQueue guarantees it),
    // so the bytes are stable without the lock.
    const bool written = writeFrame(item->binary ? ws::Opcode::Binary : ws::Opcode::Text,
                                    item->bytes, item->length);
    {
      QueueLock lock(_queueMutex);
      if (written) {
        _queue->completeSend();
      } else {
        // Partial or failed: the rest of the frame cannot follow on this
        // socket, and nothing queued can be trusted on a new one.
        _queue->failSend();
      }
    }
    if (!written) {
      ++_stats.sendFailures;
      drop(CloseCause::WriteFailed);
      return false;
    }
    ++_stats.itemsSent;
    if (kind == OutKind::TurnEnd) _turnEndSent.store(turn, std::memory_order_release);
  }
  return true;
}

void GatewayClient::handleFrame(const ws::Frame& frame) {
  switch (frame.opcode) {
    case ws::Opcode::Text:
      if (frame.length > wire::kMaxControlBytes) {
        memset(&_event, 0, sizeof(_event));
        _event.type = EventType::ProtocolError;
        _event.generation = _generation;
        _event.detail = kDetailOversizeText;
        post(false);
        closeWith(1009, CloseCause::Protocol);
        return;
      }
      memset(&_event, 0, sizeof(_event));
      _event.type = EventType::Text;
      _event.generation = _generation;
      _event.length = static_cast<uint16_t>(frame.length);
      if (frame.length > 0) memcpy(_event.text, frame.payload, frame.length);
      _event.text[frame.length] = '\0';
      if (!post(false)) {
        // The loop has fallen behind: a lost speech_start or turn_complete
        // would corrupt the turn, so reconnect instead of dropping it.
        closeWith(1011, CloseCause::Protocol);
      }
      return;

    case ws::Opcode::Binary:
      handleModelAudio(frame);
      return;

    case ws::Opcode::Ping:
      ++_stats.pingsAnswered;
      if (!writeFrame(ws::Opcode::Pong, frame.payload, frame.length)) {
        drop(CloseCause::WriteFailed);
      }
      return;

    case ws::Opcode::Pong:
    case ws::Opcode::Continuation:
      return;

    case ws::Opcode::Close: {
      const uint16_t code = ws::closeCode(frame);
      if (code == 1005) {
        writeFrame(ws::Opcode::Close, nullptr, 0);
      } else {
        const uint8_t echo[2] = {static_cast<uint8_t>(code >> 8),
                                 static_cast<uint8_t>(code & 0xFFu)};
        writeFrame(ws::Opcode::Close, echo, sizeof(echo));
      }
      _tls->stop();
      _open = false;
      postClosed(_generation, CloseCause::Peer, code);
      return;
    }
  }
}

// The downstream PRODUCER. Never waits: a frame is written whole, or refused.
void GatewayClient::handleModelAudio(const ws::Frame& frame) {
  wire::AudioFrameView view;
  const wire::FrameError error =
      wire::parseAudioFrame(frame.payload, frame.length, wire::kKindModelAudio, view);
  if (error != wire::FrameError::None) {
    ++_stats.badFrames;
    memset(&_event, 0, sizeof(_event));
    _event.type = EventType::ProtocolError;
    _event.generation = _generation;
    _event.detail = kDetailBadAudioFrame;
    post(false);
    closeWith(1002, CloseCause::Protocol);
    return;
  }
  ++_stats.downFrames;
  _stats.downBytes += view.pcmBytes;

  if (_ring.acceptedConnection() != _generation) {
    // The loop has moved to another connection already.
    ++_stats.oldConnectionFrames;
    return;
  }
  // Capacity first, then credit: a refused frame changes no count.
  if (_ring.freeBytes() < DownstreamRing::kHeaderBytes + view.pcmBytes) {
    ++_stats.capacityViolations;
    violation(view.turn, kViolationCapacity);
    return;
  }
  if (!_credit.admit(view.pcmBytes)) {
    violation(view.turn, kViolationCredit);
    return;
  }
  // Cannot fail: only the consumer frees space, so the check above still holds.
  _ring.write(_generation, view.turn, view.pcm, view.pcmBytes);
  if (_credit.gatewayRemaining() == 0) ++_stats.zeroCreditStalls;
}

void GatewayClient::violation(uint32_t turn, int32_t kind) {
  memset(&_event, 0, sizeof(_event));
  _event.type = EventType::DownstreamViolation;
  _event.generation = _generation;
  _event.turn = turn;
  _event.detail = kind;
  post(false);
  // 1008 policy violation. The Closed event that follows is what reconnects.
  closeWith(1008, CloseCause::Protocol);
}

bool GatewayClient::writeFrame(ws::Opcode opcode, const uint8_t* payload, size_t length) {
  uint8_t mask[4];
  esp_fill_random(mask, sizeof(mask));
  const size_t frameLength =
      ws::encodeClientFrame(opcode, payload, length, mask, _tx, kTxBufferBytes);
  if (frameLength == 0) return false;
  const uint32_t started = millis();
  size_t sent = 0;
  while (sent < frameLength) {
    const size_t n = _tls->write(_tx + sent, frameLength - sent);
    if (n == 0) return false;
    sent += n;
  }
  const uint32_t elapsed = millis() - started;
  if (elapsed > _stats.maxWriteMs) _stats.maxWriteMs = elapsed;
  _stats.bytesOut += static_cast<uint32_t>(frameLength);
  return true;
}

void GatewayClient::closeWith(uint16_t code, CloseCause cause) {
  const uint8_t payload[2] = {static_cast<uint8_t>(code >> 8),
                              static_cast<uint8_t>(code & 0xFFu)};
  writeFrame(ws::Opcode::Close, payload, sizeof(payload));  // best effort
  _tls->stop();
  _open = false;
  postClosed(_generation, cause, code);
}

void GatewayClient::drop(CloseCause cause) {
  _tls->stop();
  _open = false;
  postClosed(_generation, cause, 0);
}

void GatewayClient::postConnectFailed(ConnectFailure failure, int32_t detail,
                                      uint32_t startedMs) {
  memset(&_event, 0, sizeof(_event));
  _event.type = EventType::ConnectFailed;
  _event.generation = _generation;
  _event.failure = failure;
  _event.detail = detail;
  _event.elapsedMs = millis() - startedMs;
  _event.heapFree = internalFree();
  _event.heapLargest = internalLargest();
  _event.lowWaterBefore = _connectLowWater;
  _event.lowWaterAfter = internalLowWater();
  post(true);
}

void GatewayClient::postClosed(uint32_t generation, CloseCause cause, int32_t code) {
  memset(&_event, 0, sizeof(_event));
  _event.type = EventType::Closed;
  _event.generation = generation;
  _event.cause = cause;
  _event.detail = code;
  _event.elapsedMs = (_openedAtMs != 0 && generation == _generation) ? millis() - _openedAtMs : 0;
  post(true);
}

bool GatewayClient::post(bool terminal) {
  // Terminal events (connect failed, closed) end a GatewaySession wait, so
  // they get a short grace period; everything else is refused at once.
  if (xQueueSend(_events, &_event, terminal ? pdMS_TO_TICKS(200) : 0) != pdTRUE) {
    ++_stats.eventDrops;
    return false;
  }
  return true;
}

}  // namespace tth
