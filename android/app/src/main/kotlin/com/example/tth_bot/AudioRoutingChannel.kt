package com.example.tth_bot

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

/**
 * DEVELOPMENT-ONLY diagnostic (and small reinforcing-fix) channel for
 * Android audio routing during a voice session.
 *
 * Why this exists: neither the `record` nor `flutter_soloud` Flutter
 * packages expose AudioManager routing state, so there is no way to
 * confirm from Dart alone whether playback is actually reaching the
 * built-in loudspeaker versus the quiet earpiece. This is deliberately the
 * smallest native surface that can read that state and, on API 31+,
 * explicitly request the loudspeaker via the current official
 * AudioManager communication-device API. See
 * lib/features/voice/android_audio_routing.dart for the Dart side and the
 * Phase 4B stabilization report for the diagnosis this exists to confirm.
 *
 * The actual routing *fix* for API < 31 is MicrophoneService's
 * `speakerphone: true` config, handled entirely inside the `record`
 * plugin (AudioManager.setSpeakerphoneOn) — this class does not duplicate
 * that; `forceSpeakerRouting` is a no-op below API 31.
 */
class AudioRoutingChannel(private val context: Context) : MethodChannel.MethodCallHandler {
    companion object {
        const val CHANNEL_NAME = "edubot/audio_routing"
    }

    private val audioManager: AudioManager
        get() = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "getRoutingInfo" -> result.success(getRoutingInfo())
            "forceSpeakerRouting" -> result.success(forceSpeakerRouting())
            "clearForcedRouting" -> {
                clearForcedRouting()
                result.success(null)
            }
            else -> result.notImplemented()
        }
    }

    @Suppress("DEPRECATION")
    private fun getRoutingInfo(): Map<String, Any?> {
        val am = audioManager
        val info = mutableMapOf<String, Any?>(
            "sdkInt" to Build.VERSION.SDK_INT,
            "mode" to modeToString(am.mode),
            "isSpeakerphoneOn" to am.isSpeakerphoneOn,
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            info["communicationDevice"] = am.communicationDevice?.let { deviceTypeToString(it.type) }
            info["availableCommunicationDevices"] =
                am.availableCommunicationDevices.map { deviceTypeToString(it.type) }
        }
        return info
    }

    /**
     * Best-effort request to route communication audio to the built-in
     * loudspeaker via the modern (API 31+) AudioManager API. Returns
     * whether it actually reports success — per the task's explicit "do
     * not assume routing succeeded merely because a method returned",
     * [setCommunicationDevice]'s own return value is used, not just the
     * absence of an exception.
     */
    private fun forceSpeakerRouting(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            // Pre-API 31: MicrophoneService's `speakerphone: true` (via the
            // `record` plugin's own setSpeakerphoneOn call) is the only
            // mechanism available; nothing extra to do here.
            return false
        }
        val am = audioManager
        val speaker = am.availableCommunicationDevices.firstOrNull {
            it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
        } ?: return false
        return am.setCommunicationDevice(speaker)
    }

    private fun clearForcedRouting() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            audioManager.clearCommunicationDevice()
        }
    }

    @Suppress("DEPRECATION")
    private fun modeToString(mode: Int): String = when (mode) {
        AudioManager.MODE_NORMAL -> "MODE_NORMAL"
        AudioManager.MODE_RINGTONE -> "MODE_RINGTONE"
        AudioManager.MODE_IN_CALL -> "MODE_IN_CALL"
        AudioManager.MODE_IN_COMMUNICATION -> "MODE_IN_COMMUNICATION"
        else -> "MODE_UNKNOWN($mode)"
    }

    private fun deviceTypeToString(type: Int): String = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "BUILTIN_SPEAKER"
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "BUILTIN_EARPIECE"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "BLUETOOTH_SCO"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "BLUETOOTH_A2DP"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "WIRED_HEADSET"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "WIRED_HEADPHONES"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB_HEADSET"
        else -> "TYPE_$type"
    }
}
