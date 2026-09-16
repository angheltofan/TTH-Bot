#include "audio/M5AudioDevice.h"

#include <M5Unified.h>

#include "diag/BlockTimer.h"
#include "tth/Config.h"
#include "tth/SpeakerVolume.h"

namespace tth {

void M5AudioDevice::configure() {
  // The microphone must run at the same rate the Flutter app captures at, so
  // both embodiments feed the gateway identical audio later on.
  auto micCfg = M5.Mic.config();
  micCfg.sample_rate = TTH_MIC_SAMPLE_RATE;
  M5.Mic.config(micCfg);

  // Volume only. No speaker sample rate is set here, deliberately: M5Unified
  // takes the rate per playRaw() call, so each stream plays at its own
  // declared rate (16 kHz loopback, 24 kHz assistant speech).
  setMasterVolume(TTH_SPEAKER_VOLUME);
}

void M5AudioDevice::setMasterVolume(uint32_t volume) {
  M5.Speaker.setVolume(clampSpeakerVolume(volume));
}

uint8_t M5AudioDevice::masterVolume() const { return M5.Speaker.getVolume(); }

bool M5AudioDevice::micBegin() {
  TTH_TIME_BLOCK("M5.Mic.begin");
  const bool ok = M5.Mic.begin();
  if (!ok) {
    Serial.println(F("[audio] M5.Mic.begin() FAILED"));
  }
  return ok;
}

void M5AudioDevice::micEnd() {
  TTH_TIME_BLOCK("M5.Mic.end");
  // No isEnabled() guard here on purpose. AudioBus only calls this for a
  // microphone whose begin() previously returned true, so an unmatched
  // uninstall cannot reach this point. Adding a defensive guard would hide a
  // future AudioBus bug rather than surface it.
  M5.Mic.end();
}

bool M5AudioDevice::speakerBegin() {
  TTH_TIME_BLOCK("M5.Speaker.begin");
  const bool ok = M5.Speaker.begin();
  if (!ok) {
    Serial.println(F("[audio] M5.Speaker.begin() FAILED"));
  }
  return ok;
}

void M5AudioDevice::speakerEnd() {
  TTH_TIME_BLOCK("M5.Speaker.end");
  M5.Speaker.end();
}

}  // namespace tth
