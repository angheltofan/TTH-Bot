// TTH Bot - M5Stack Core2 firmware entry point.
//
// Deliberately thin: all behaviour lives in tth::App so that the Arduino
// setup()/loop() shape stays an implementation detail of the platform rather
// than a place logic accumulates.
//
// PHASE 1 SCOPE. Present: M5Unified init, startup diagnostics, the AudioBus
// ownership boundary, and a cooperative non-blocking loop. Absent by design:
// the robot face, push-to-talk behaviour, audio capture/playback, Wi-Fi,
// WebSocket, Gemini and Supabase. No such header is included anywhere in this
// project yet.

#include "app/App.h"

namespace {
tth::App g_app;
}  // namespace

void setup() { g_app.begin(); }

void loop() { g_app.tick(); }
