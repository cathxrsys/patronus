package org.patronus.client;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.MediaPlayer;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;

// Loops the ringtone and vibration for an incoming call that was surfaced by an
// FCM push while the app itself is not yet on screen ringing.
//
// It is only used when the device is unlocked and interactive: there the
// full-screen intent merely shows a heads-up banner and does NOT launch the app,
// so nothing would otherwise ring until the user taps. On a locked or asleep
// device the full-screen intent launches the app, which rings itself — so this
// ringer is deliberately NOT started there (avoids double ringing).
//
// Once the app comes online and presents the call (or the call is cancelled /
// answered / declined) stop() hands ringing over / silences it.
final class IncomingCallRinger {
    private static final String TAG = "PatronusFCM";

    // [wait, vibrate, pause] repeated from index 0 -> continuous call-like buzz.
    private static final long[] VIBRATION_PATTERN = { 0, 900, 600 };

    private static MediaPlayer player;
    private static Vibrator vibrator;

    private IncomingCallRinger() {
    }

    static synchronized void start(Context context) {
        stop();

        AudioManager audioManager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        int ringerMode = audioManager != null ? audioManager.getRingerMode() : AudioManager.RINGER_MODE_NORMAL;

        if (ringerMode == AudioManager.RINGER_MODE_NORMAL) {
            startRingtone(context);
        }
        if (ringerMode != AudioManager.RINGER_MODE_SILENT) {
            startVibration(context);
        }
    }

    static synchronized void stop() {
        if (player != null) {
            try {
                if (player.isPlaying()) {
                    player.stop();
                }
            } catch (Exception ignored) {
            }
            try {
                player.release();
            } catch (Exception ignored) {
            }
            player = null;
        }
        if (vibrator != null) {
            try {
                vibrator.cancel();
            } catch (Exception ignored) {
            }
            vibrator = null;
        }
    }

    private static void startRingtone(Context context) {
        try {
            Uri ringtone = appRingtoneUri(context);
            if (ringtone == null) {
                ringtone = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);
            }
            if (ringtone == null) {
                return;
            }
            player = new MediaPlayer();
            player.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build());
            player.setDataSource(context, ringtone);
            player.setLooping(true);
            player.prepare();
            player.start();
        } catch (Exception e) {
            Log.e(TAG, "IncomingCallRinger ringtone failed: " + e.getMessage());
            if (player != null) {
                try {
                    player.release();
                } catch (Exception ignored) {
                }
                player = null;
            }
        }
    }

    private static void startVibration(Context context) {
        vibrator = resolveVibrator(context);
        if (vibrator == null || !vibrator.hasVibrator()) {
            vibrator = null;
            return;
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                vibrator.vibrate(VibrationEffect.createWaveform(VIBRATION_PATTERN, 0));
            } else {
                vibrator.vibrate(VIBRATION_PATTERN, 0);
            }
        } catch (Exception e) {
            Log.e(TAG, "IncomingCallRinger vibration failed: " + e.getMessage());
            vibrator = null;
        }
    }

    // The app's own ringtone bundled in res/raw, so the notification ring matches
    // the in-app one. Resolved by name (no compile-time R dependency); returns
    // null if it isn't packaged, in which case the caller falls back to the
    // system ringtone.
    private static Uri appRingtoneUri(Context context) {
        try {
            int resId = context.getResources().getIdentifier(
                "incoming_call_default", "raw", context.getPackageName());
            if (resId == 0) {
                return null;
            }
            return Uri.parse("android.resource://" + context.getPackageName() + "/" + resId);
        } catch (Exception e) {
            return null;
        }
    }

    private static Vibrator resolveVibrator(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            VibratorManager manager = (VibratorManager) context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            return manager != null ? manager.getDefaultVibrator() : null;
        }
        return (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
    }
}
