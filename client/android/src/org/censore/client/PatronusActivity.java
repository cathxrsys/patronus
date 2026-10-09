package org.patronus.client;

import android.app.Activity;
import android.app.KeyguardManager;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.Context;
import android.content.ContentValues;
import android.content.IntentFilter;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Insets;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.media.MediaPlayer;
import android.media.RingtoneManager;
import android.os.Bundle;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.PowerManager;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.provider.OpenableColumns;
import android.provider.MediaStore;
import android.graphics.Rect;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsAnimation;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.view.ViewGroup;

import java.lang.reflect.Field;

import org.qtproject.qt.android.bindings.QtActivity;

import androidx.core.content.FileProvider;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URLConnection;
import java.util.List;

public class PatronusActivity extends QtActivity {
    private static final int OPEN_FILE_REQUEST_CODE = 1001;
    private static final int OPEN_FILES_REQUEST_CODE = 1003;
    private static final int NOTIFICATION_PERMISSION_REQUEST_CODE = 1002;
    private static final String TAG = "PatronusFCM";

    // Set on the launch intent of the full-screen incoming-call notification so
    // the activity knows to show over the lockscreen and turn the screen on.
    public static final String EXTRA_INCOMING_CALL = "incoming_call";

    // Carried by the Accept/Decline notification actions; the app auto-performs
    // the action once the call is on screen (see CallManager).
    public static final String EXTRA_CALL_ACTION = "call_action";
    public static final String CALL_ACTION_ACCEPT = "accept";
    public static final String CALL_ACTION_DECLINE = "decline";

    // Action requested from a notification button, awaiting the call to be shown.
    // Read once (and cleared) by native via consumePendingCallAction().
    private static volatile String pendingCallAction = null;

    static void clearPendingCallAction() {
        pendingCallAction = null;
    }

    // True when this activity was brought up by the incoming-call notification
    // (over the lockscreen). If such a call ends without being answered, we send
    // the task back so the device returns to its locked/asleep state instead of
    // leaving the app on screen. Cleared once the call is answered or dismissed.
    private boolean launchedForCall = false;
    private boolean callAnswered = false;
    // Set on any touch/key while the call screen is up; if the user is actively
    // interacting we keep the app open even on an unanswered cancel.
    private volatile boolean userInteractedDuringCall = false;

    private static native void nativeOnFilePicked(String fileName, String filePath);
    private static native void nativeOnFilePickCancelled();
    private static native void nativeOnFilesPicked(String[] fileNames, String[] filePaths);
    private static native void nativeOnFilesPickCancelled();
    private static native void nativeOnKeyboardHeightChanged(int heightPixels);
    private static native void nativeOnSpeakerphoneChanged(boolean enabled);
    private static native void nativeOnFcmTokenReady(String serverKey, String token);
    private static native void nativeOnFcmTokenError(String serverKey, String errorText);
    private static native void nativeOnCallActionFromNotification();

    // Cyclic vibration patterns for incoming/outgoing calls: [off_ms, on_ms, off_ms, on_ms, ...]
    private static final long[][] CALL_VIBRATION_PATTERNS = {
        new long[]{ 0, 900, 600, 900 },             // 0: Standard
        new long[]{ 0, 1200, 700, 1200 },           // 1: Slow
        new long[]{ 0, 500, 300, 500, 300, 500 },  // 2: Triple
    };

    private AudioManager audioManager;
    private PowerManager powerManager;
    private PowerManager.WakeLock proximityWakeLock;
    private Vibrator vibrator;
    private boolean callAudioActive;
    private boolean callProximityActive;
    private boolean speakerphoneEnabled;
    private MediaPlayer ringtonePlayer;
    private boolean ringtoneUsesEarpiece;
    private final BroadcastReceiver fcmTokenReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent == null || !ServerFirebaseManager.ACTION_FCM_TOKEN_UPDATED.equals(intent.getAction())) {
                return;
            }

            String serverKey = intent.getStringExtra(ServerFirebaseManager.EXTRA_SERVER_KEY);
            String token = intent.getStringExtra(ServerFirebaseManager.EXTRA_TOKEN);
            String errorText = intent.getStringExtra(ServerFirebaseManager.EXTRA_ERROR);
            Log.d(TAG, "Received FCM broadcast serverKey=" + serverKey
                    + " tokenLen=" + (token != null ? token.length() : 0)
                    + " hasError=" + (errorText != null && !errorText.isEmpty()));
            if (errorText != null && !errorText.isEmpty()) {
                nativeOnFcmTokenError(serverKey, errorText);
                return;
            }
            if (token != null && !token.isEmpty()) {
                nativeOnFcmTokenReady(serverKey, token);
            }
        }
    };

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.d(TAG, "Activity created");
        audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
        powerManager = (PowerManager) getSystemService(POWER_SERVICE);

        // Register notification channel early so FCM auto-display uses our HIGH importance channel
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) {
            NotificationHelper.ensureNotificationChannel(nm);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            VibratorManager vm = (VibratorManager) getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            vibrator = vm != null ? vm.getDefaultVibrator() : null;
        } else {
            vibrator = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
        }
        registerFcmReceiver();
        requestNotificationPermissionIfNeeded();
        setupKeyboardTracking();
        handleIncomingCallIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIncomingCallIntent(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Entering (or returning to) the app: clear the pile of message
        // notifications from the tray so the user does not have to swipe them
        // away by hand. A live incoming-call notification is preserved.
        NotificationHelper.clearMessageNotifications(this);
    }

    @Override
    public void onUserInteraction() {
        super.onUserInteraction();
        // The user is touching the screen during a call we launched for; don't
        // auto-close the app if the call later ends unanswered.
        if (launchedForCall) {
            userInteractedDuringCall = true;
        }
    }

    // When launched from the full-screen incoming-call notification, show over
    // the lockscreen and turn the screen on so the user sees the call right away.
    private void handleIncomingCallIntent(Intent intent) {
        if (intent == null || !intent.getBooleanExtra(EXTRA_INCOMING_CALL, false)) {
            return;
        }
        String action = intent.getStringExtra(EXTRA_CALL_ACTION);
        if (action != null && !action.isEmpty()) {
            // Queue it; CallManager applies it once the call is presented.
            pendingCallAction = action;
            Log.d(TAG, "handleIncomingCallIntent: queued call action=" + action);
            // Poke native in case the call is ALREADY on screen (the app was
            // woken and showed the call before the user tapped) — otherwise the
            // action would sit unconsumed. No-op if native isn't up yet (cold
            // start): presentCallRequest will consume it when the call appears.
            try {
                nativeOnCallActionFromNotification();
            } catch (Throwable t) {
                Log.w(TAG, "nativeOnCallActionFromNotification failed: " + t.getMessage());
            }
        }
        if (launchedForCall && callAnswered) {
            // Redelivered incoming-call intent (FCM redelivers "at least once", or
            // Android replays the notification's full-screen PendingIntent) for a
            // call we already answered and that is still in progress. Do NOT reset
            // the answered/interaction flags here: that would make the call's
            // normal end look "unanswered" to dismissCallScreenIfLaunchedForCall()
            // and kill the app mid-call. The flags are reset below for a genuinely
            // new call (by then the previous one is no longer launchedForCall).
            Log.d(TAG, "handleIncomingCallIntent: redelivered for an already-answered call, keeping state");
            return;
        }
        launchedForCall = true;
        callAnswered = false;
        userInteractedDuringCall = false;
        Log.d(TAG, "handleIncomingCallIntent: launched for incoming call");
        runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
                setShowWhenLocked(true);
                setTurnScreenOn(true);
                KeyguardManager keyguardManager = (KeyguardManager) getSystemService(KEYGUARD_SERVICE);
                if (keyguardManager != null) {
                    keyguardManager.requestDismissKeyguard(this, null);
                }
            } else {
                getWindow().addFlags(
                    WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED
                        | WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON
                        | WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD);
            }
        });
    }

    // Called from native (AndroidSystemUi) once the call_request is decrypted and
    // the caller resolved, to relabel the FCM-posted notification with their name.
    public void updateIncomingCallNotification(final String callerName) {
        runOnUiThread(() -> NotificationHelper.updateIncomingCallNotification(this, callerName));
    }

    // Called from native when the call ends/answered/declined so the incoming
    // call notification is dismissed.
    public void cancelIncomingCallNotification() {
        runOnUiThread(() -> NotificationHelper.cancelIncomingCallNotification(this));
    }

    // Called from native when the user opens a chat, to sweep away the message
    // notifications from the tray so they don't linger after the messages have
    // been read. Any live incoming-call notification is preserved.
    public void clearMessageNotifications() {
        runOnUiThread(() -> NotificationHelper.clearMessageNotifications(this));
    }

    // Called from native when an incoming call is presented, to find out whether
    // the user already chose Accept/Decline from the notification. Returns the
    // action ("accept"/"decline") and clears it, or "" if none.
    public String consumePendingCallAction() {
        String action = pendingCallAction;
        pendingCallAction = null;
        return action != null ? action : "";
    }

    // True while an FCM incoming-call notification is live, so the call UI knows
    // it was surfaced from a push and must not start its own (duplicate) ringtone.
    public boolean isIncomingCallNotificationActive() {
        return NotificationHelper.isIncomingCallActive();
    }

    // Native marks the call answered so a later end-of-call does not send the app
    // to the background (the user is engaged in the call).
    public void markCallAnswered() {
        callAnswered = true;
    }

    // Native calls this when an incoming call ends without being answered. If this
    // activity was opened by the call (over the lockscreen) and the user took no
    // answering action, terminate the app entirely so the websocket is closed and
    // the server marks us offline — leaving it open hangs the socket and corrupts
    // the ratchet. The next call/message arrives via FCM. No-op if the call was
    // answered or the app was already open (a live, non-FCM call).
    public void dismissCallScreenIfLaunchedForCall() {
        runOnUiThread(() -> {
            if (!launchedForCall || callAnswered || userInteractedDuringCall) {
                launchedForCall = false;
                return;
            }
            launchedForCall = false;
            Log.d(TAG, "unanswered FCM call: closing app to release the socket");
            // Stop holding the screen on and hide the app so the device returns to
            // its locked/asleep state.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
                setShowWhenLocked(false);
                setTurnScreenOn(false);
            } else {
                getWindow().clearFlags(
                    WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED
                        | WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON
                        | WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD);
            }
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            moveTaskToBack(true);
            // Terminate after a short grace period (the missed-call DB write is
            // already flushed synchronously) so the OS tears down the websocket.
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(
                () -> android.os.Process.killProcess(android.os.Process.myPid()), 700);
        });
    }

    @Override
    protected void onDestroy() {
        Log.d(TAG, "Activity destroyed");
        stopRingtoneInternal();
        unregisterFcmReceiver();
        applyCallAudioModeImmediate(false);
        applyCallAudioActiveUi(false);
        super.onDestroy();
    }

    public void startRingtone(final String filePath, final boolean useEarpiece) {
        runOnUiThread(() -> {
            stopRingtoneInternal();
            ringtoneUsesEarpiece = useEarpiece;
            try {
                ringtonePlayer = new MediaPlayer();
                if (useEarpiece) {
                    if (audioManager != null)
                        audioManager.setMode(AudioManager.MODE_IN_COMMUNICATION);
                    ringtonePlayer.setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build());
                    ringtonePlayer.setDataSource(filePath);
                    setVolumeControlStream(AudioManager.STREAM_VOICE_CALL);
                } else {
                    ringtonePlayer.setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build());
                    Uri systemRingtoneUri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);
                    ringtonePlayer.setDataSource(PatronusActivity.this, systemRingtoneUri);
                    setVolumeControlStream(AudioManager.STREAM_RING);
                }
                ringtonePlayer.setLooping(true);
                ringtonePlayer.prepare();
                ringtonePlayer.start();
            } catch (Exception e) {
                Log.e(TAG, "startRingtone failed: " + e.getMessage());
                if (ringtonePlayer != null) {
                    ringtonePlayer.release();
                    ringtonePlayer = null;
                }
                ringtoneUsesEarpiece = false;
            }
        });
    }

    public void stopRingtone() {
        runOnUiThread(() -> {
            boolean wasEarpiece = ringtoneUsesEarpiece;
            ringtoneUsesEarpiece = false;
            stopRingtoneInternal();
            if (!callAudioActive) {
                setVolumeControlStream(AudioManager.USE_DEFAULT_STREAM_TYPE);
                if (wasEarpiece && audioManager != null)
                    audioManager.setMode(AudioManager.MODE_NORMAL);
            }
        });
    }

    private void stopRingtoneInternal() {
        if (ringtonePlayer == null) return;
        try {
            if (ringtonePlayer.isPlaying()) ringtonePlayer.stop();
            ringtonePlayer.release();
        } catch (Exception ignored) {}
        ringtonePlayer = null;
    }

    public void startCallVibration(int patternIndex) {
        if (vibrator == null || !vibrator.hasVibrator()) return;
        long[] pattern = (patternIndex >= 0 && patternIndex < CALL_VIBRATION_PATTERNS.length)
            ? CALL_VIBRATION_PATTERNS[patternIndex]
            : CALL_VIBRATION_PATTERNS[0];
        runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                vibrator.vibrate(VibrationEffect.createWaveform(pattern, 2));
            } else {
                vibrator.vibrate(pattern, 2);
            }
        });
    }

    public void stopCallVibration() {
        if (vibrator != null) {
            runOnUiThread(vibrator::cancel);
        }
    }

    public void showSystemNotification(String title, String message) {
        runOnUiThread(() -> NotificationHelper.showSystemNotification(this, title, message));
    }

    private void registerFcmReceiver() {
        IntentFilter filter = new IntentFilter(ServerFirebaseManager.ACTION_FCM_TOKEN_UPDATED);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(fcmTokenReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(fcmTokenReceiver, filter);
        }
    }

    private void unregisterFcmReceiver() {
        try {
            unregisterReceiver(fcmTokenReceiver);
        } catch (IllegalArgumentException ignored) {
        }
    }

    private void setupKeyboardTracking() {
        final Window window = getWindow();
        if (window == null) {
            return;
        }

        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);

        final View decorView = window.getDecorView();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Catches non-animated IME height changes (e.g. SwiftKey T9 bar appearing
            // after the keyboard animation finishes at 736px → jumps to 944px).
            // WindowInsetsAnimationCallback.onProgress only fires for animated changes;
            // non-animated inset updates come through applyWindowInsets instead.
            decorView.setOnApplyWindowInsetsListener((v, insets) -> {
                if (insets.isVisible(WindowInsets.Type.ime())) {
                    Insets imeI = insets.getInsets(WindowInsets.Type.ime());
                    Insets navI = insets.getInsets(WindowInsets.Type.navigationBars());
                    int h = Math.max(0, imeI.bottom - navI.bottom);
                    // Suppress transient dips while keyboard is visible (e.g. SwiftKey
                    // 944→840→944 bounce during restartInput). Only forward increases.
                    if (h >= mLastApplyInsetsH) {
                        Log.d(TAG_NC, "applyInsets: h=" + h);
                        mLastApplyInsetsH = h;
                        nativeOnKeyboardHeightChanged(h);
                    } else {
                        Log.d(TAG_NC, "applyInsets: h=" + h + " suppressed (dip from " + mLastApplyInsetsH + ")");
                    }
                } else {
                    mLastApplyInsetsH = 0; // reset for next keyboard show
                }
                return v.onApplyWindowInsets(insets);
            });

            decorView.setWindowInsetsAnimationCallback(
                new WindowInsetsAnimation.Callback(WindowInsetsAnimation.Callback.DISPATCH_MODE_STOP) {
                    private int mProgressCount = 0;
                    private int mLastLoggedH = -1;

                    @Override
                    public WindowInsets onProgress(WindowInsets insets, List<WindowInsetsAnimation> runningAnimations) {
                        Insets imeInsets = insets.getInsets(WindowInsets.Type.ime());
                        Insets navInsets = insets.getInsets(WindowInsets.Type.navigationBars());
                        int keyboardHeight = Math.max(0, imeInsets.bottom - navInsets.bottom);
                        mProgressCount++;
                        if (mProgressCount <= 3 || keyboardHeight != mLastLoggedH) {
                            Log.d(TAG_NC, "onProgress #" + mProgressCount + " h=" + keyboardHeight);
                            mLastLoggedH = keyboardHeight;
                        }
                        nativeOnKeyboardHeightChanged(keyboardHeight);
                        return insets;
                    }

                    @Override
                    public WindowInsetsAnimation.Bounds onStart(WindowInsetsAnimation animation, WindowInsetsAnimation.Bounds bounds) {
                        super.onStart(animation, bounds);
                        mProgressCount = 0;
                        mLastLoggedH = -1;
                        Log.d(TAG_NC, "onStart animType=0x" + Integer.toHexString(animation.getTypeMask()));
                        return bounds;
                    }

                    @Override
                    public void onEnd(WindowInsetsAnimation animation) {
                        super.onEnd(animation);
                        WindowInsets insets = decorView.getRootWindowInsets();
                        if (insets != null) {
                            Insets imeInsets = insets.getInsets(WindowInsets.Type.ime());
                            Insets navInsets = insets.getInsets(WindowInsets.Type.navigationBars());
                            int keyboardHeight = Math.max(0, imeInsets.bottom - navInsets.bottom);
                            Log.d(TAG_NC, "onEnd h=" + keyboardHeight + " progressCalls=" + mProgressCount);
                            nativeOnKeyboardHeightChanged(keyboardHeight);
                            // Keyboard animation finished — now safe to do IC wrap + restartInput
                            // without interrupting the animation. T9 bar will appear right after.
                            if (mIcWrapPending && insets.isVisible(WindowInsets.Type.ime())) {
                                mIcWrapPending = false;
                                final InputMethodManager immLocal = mImm;
                                if (immLocal != null) {
                                    new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(
                                        () -> scheduleIcWrap(immLocal, 0), 50);
                                }
                            }
                        }
                    }
                }
            );
        } else {
            decorView.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
                Rect visibleFrame = new Rect();
                decorView.getWindowVisibleDisplayFrame(visibleFrame);
                int screenHeight = decorView.getRootView().getHeight();
                int keyboardHeight = Math.max(0, screenHeight - visibleFrame.bottom);
                nativeOnKeyboardHeightChanged(keyboardHeight);
            });
        }
    }

    public void applySystemBarStyle(int color, boolean darkIcons) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
            return;
        }

        final Window window = getWindow();
        if (window == null) {
            return;
        }

        runOnUiThread(() -> {
            window.setStatusBarColor(color);
            // Without this the nav bar stays the system default (opaque black on
            // most devices/emulators), which visibly seams against any theme
            // background that isn't itself near-neutral black.
            window.setNavigationBarColor(color);

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                final View decorView = window.getDecorView();
                int flags = decorView.getSystemUiVisibility();
                if (darkIcons) {
                    flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
                } else {
                    flags &= ~View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    if (darkIcons) {
                        flags |= View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
                    } else {
                        flags &= ~View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
                    }
                }
                decorView.setSystemUiVisibility(flags);
            }
        });
    }

    public void restartInput() {
        runOnUiThread(() -> {
            View view = getWindow().getDecorView().findFocus();
            if (view == null) {
                view = getWindow().getDecorView();
            }
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.restartInput(view);
            }
        });
    }

    public void invalidateInput() {
        runOnUiThread(() -> {
            Window window = getWindow();
            if (window == null) {
                return;
            }

            View view = window.getDecorView().findFocus();
            if (view == null) {
                view = window.getDecorView();
            }

            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm == null) {
                return;
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                imm.invalidateInput(view);
            } else {
                imm.restartInput(view);
            }
        });
    }

    // Qt ImhFlags bitmask passed by QML when showing the keyboard, used to
    // restore m_imeOptions/m_inputType in scheduleIcWrap (Qt resets these to
    // defaults when keyboard hides; without restoration Enter hides keyboard
    // instead of inserting a newline in multiline TextArea).
    private volatile int mPendingImhHints = 0;
    private volatile boolean mIcWrapPending = false;
    private volatile InputMethodManager mImm = null;
    // Tracks the highest h seen via applyInsets while IME is visible so we can
    // suppress transient dips caused by restartInput (e.g. SwiftKey 944→840→944).
    private int mLastApplyInsetsH = 0;

    // Qt ImhFlags → Android inputType/imeOptions.
    // Qt 6 enum values (qnamespace.h): ImhMultiLine=0x400, ImhHiddenText=0x1,
    // ImhSensitiveData=0x2, ImhNoAutoUppercase=0x4, ImhNoPredictiveText=0x40.
    // (Qt 5 used different values like ImhMultiLine=0x4000 — do not use those.)
    private static int imhToAndroidInputType(int imhHints) {
        int inputType = 0x00000001; // TYPE_CLASS_TEXT
        if ((imhHints & 0x400) != 0) { // Qt 6: ImhMultiLine = 0x400
            inputType |= 0x00020000;   // TYPE_TEXT_FLAG_MULTI_LINE
        }
        return inputType;
    }
    private static int imhToAndroidImeOptions(int imhHints) {
        if ((imhHints & 0x400) != 0) { // Qt 6: ImhMultiLine = 0x400
            return 0x40000000;          // IME_FLAG_NO_ENTER_ACTION
        }
        return 0;
    }

    public void showSoftKeyboardWithHints(int imhHints) {
        Log.d(TAG_NC, "showSoftKeyboardWithHints hints=0x" + Integer.toHexString(imhHints)
                + " multiLine=" + ((imhHints & 0x400) != 0));
        mPendingImhHints = imhHints;
        showSoftKeyboard();
    }

    public void showSoftKeyboard() {
        runOnUiThread(() -> {
            Window window = getWindow();
            if (window == null) return;
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm == null) return;
            View decorView = window.getDecorView();

            // Use QtEditText as focus target so mServedView == qtEditText == wrapped,
            // which is required for invalidateInput/restartInput to be effective.
            View qtEt = findQtEditTextView();
            View target = qtEt != null ? qtEt : (decorView.findFocus() != null ? decorView.findFocus() : decorView);
            // requestFocus triggers Qt lambda$2 synchronously (resets m_imeOptions to 0).
            // showSoftInput sets mServedView = target via checkFocus (sync).
            target.requestFocus();
            imm.showSoftInput(target, InputMethodManager.SHOW_IMPLICIT);

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                // IC wrap happens HERE — inside the runOnUiThread block, BEFORE
                // controller.show(ime()) starts the keyboard animation.
                // All operations in this block execute while keyboard is still hidden
                // (applyInsets only fires when isVisible(ime)=true, which only becomes true
                // after this block returns and Android processes the show request).
                // Therefore any invalidateInput/restartInput T9 bounce is fully invisible.
                if (qtEt != null) {
                    View wrapped = wrapQtInputConnection();
                    if (wrapped != null) {
                        try {
                            Class<?> cls = Class.forName("org.qtproject.qt.android.QtEditText");
                            int hints = mPendingImhHints;
                            Field f_it = cls.getDeclaredField("m_inputType");
                            Field f_io = cls.getDeclaredField("m_imeOptions");
                            Field f_oc = cls.getDeclaredField("m_optionsChanged");
                            f_it.setAccessible(true);
                            f_io.setAccessible(true);
                            f_oc.setAccessible(true);
                            f_it.setInt(wrapped, imhToAndroidInputType(hints));
                            f_io.setInt(wrapped, imhToAndroidImeOptions(hints));
                            f_oc.setBoolean(wrapped, true);
                            Log.d(TAG_NC, "Pre-show wrap: inputType=0x"
                                    + Integer.toHexString(imhToAndroidInputType(hints))
                                    + " imeOptions=0x" + Integer.toHexString(imhToAndroidImeOptions(hints)));
                        } catch (Exception e) {
                            Log.w(TAG_NC, "Pre-show EditorInfo fix failed: " + e);
                        }
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            imm.invalidateInput(wrapped);
                            Log.d(TAG_NC, "Pre-show wrap done via invalidateInput");
                        } else {
                            imm.restartInput(wrapped);
                            Log.d(TAG_NC, "Pre-show wrap done via restartInput");
                        }
                        mIcWrapPending = false; // onEnd fallback not needed
                    } else {
                        Log.d(TAG_NC, "Pre-show wrap: wrapQtInputConnection=null, fallback to onEnd");
                        mImm = imm;
                        mIcWrapPending = true;
                    }
                } else {
                    Log.d(TAG_NC, "Pre-show wrap: qtEt not found, fallback to onEnd");
                    mImm = imm;
                    mIcWrapPending = true;
                }

                // Start keyboard animation AFTER IC wrap is complete.
                WindowInsetsController controller = decorView.getWindowInsetsController();
                if (controller != null) {
                    controller.show(WindowInsets.Type.ime());
                }
            } else {
                scheduleIcWrap(imm, 0);
            }
        });
    }

    public void setCallAudioActive(boolean active) {
        // AudioManager operations are thread-safe and must run before Qt creates
        // QAudioSource/QAudioSink — so we do them synchronously here (Qt thread),
        // without runOnUiThread, to avoid the race where audio objects are created
        // before MODE_IN_COMMUNICATION is set.
        applyCallAudioModeImmediate(active);
        // Window flags and volume stream require the UI thread.
        runOnUiThread(() -> applyCallAudioActiveUi(active));
    }

    public void setCallProximityActive(boolean active) {
        runOnUiThread(() -> applyCallProximityActive(active));
    }

    public void setSpeakerphoneEnabled(boolean enabled) {
        applySpeakerphoneRoute(enabled);
    }

    private synchronized void applyCallAudioModeImmediate(boolean active) {
        if (audioManager == null || callAudioActive == active) {
            return;
        }

        callAudioActive = active;

        if (active) {
            audioManager.setMode(AudioManager.MODE_IN_COMMUNICATION);
            applySpeakerphoneRoute(false);
            return;
        }

        callProximityActive = false;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            audioManager.clearCommunicationDevice();
        } else {
            audioManager.setSpeakerphoneOn(false);
        }
        audioManager.setMode(AudioManager.MODE_NORMAL);

        if (speakerphoneEnabled) {
            speakerphoneEnabled = false;
            nativeOnSpeakerphoneChanged(false);
        }

        updateProximityWakeLock();
    }

    private void applyCallAudioActiveUi(boolean active) {
        Window w = getWindow();
        if (w == null) {
            return;
        }

        if (active) {
            w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            setVolumeControlStream(AudioManager.STREAM_VOICE_CALL);
        } else {
            releaseProximityWakeLock(false);
            w.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            setVolumeControlStream(AudioManager.USE_DEFAULT_STREAM_TYPE);
        }
    }

    private void applyCallProximityActive(boolean active) {
        if (callProximityActive == active) {
            return;
        }

        callProximityActive = active;
        updateProximityWakeLock();
    }

    private void applySpeakerphoneRoute(boolean enabled) {
        if (audioManager == null) {
            return;
        }

        boolean actualEnabled = enabled;

        if (!callAudioActive && !enabled) {
            updateProximityWakeLock();
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            audioManager.setSpeakerphoneOn(enabled);

            if (enabled) {
                AudioDeviceInfo speakerDevice = findCommunicationDevice(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
                if (speakerDevice != null) {
                    boolean applied = audioManager.setCommunicationDevice(speakerDevice);
                    if (!applied) {
                        audioManager.clearCommunicationDevice();
                    }
                } else {
                    audioManager.clearCommunicationDevice();
                }
                actualEnabled = true;
            } else {
                // Earpiece is the default output in MODE_IN_COMMUNICATION when no device is
                // explicitly set. Avoid setCommunicationDevice(EARPIECE) because it only routes
                // STREAM_VOICE_CALL, not Qt's STREAM_MUSIC, and may cause Qt's defaultAudioOutput()
                // to return the earpiece (which doesn't support 48 kHz), silently breaking QAudioSink.
                audioManager.clearCommunicationDevice();
                actualEnabled = !hasBuiltinEarpiece();
            }
        } else {
            actualEnabled = enabled || !hasBuiltinEarpiece();
            audioManager.setSpeakerphoneOn(actualEnabled);
        }

        audioManager.setMode(AudioManager.MODE_IN_COMMUNICATION);

        if (speakerphoneEnabled != actualEnabled) {
            speakerphoneEnabled = actualEnabled;
            nativeOnSpeakerphoneChanged(actualEnabled);
        }

        updateProximityWakeLock();
    }

    private AudioDeviceInfo findCommunicationDevice(int type) {
        if (audioManager == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return null;
        }

        for (AudioDeviceInfo device : audioManager.getAvailableCommunicationDevices()) {
            if (device.getType() == type) {
                return device;
            }
        }

        return null;
    }

    private boolean hasBuiltinEarpiece() {
        if (audioManager == null) {
            return getPackageManager().hasSystemFeature(PackageManager.FEATURE_TELEPHONY);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return findCommunicationDevice(AudioDeviceInfo.TYPE_BUILTIN_EARPIECE) != null;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            for (AudioDeviceInfo device : audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE) {
                    return true;
                }
            }
        }

        return getPackageManager().hasSystemFeature(PackageManager.FEATURE_TELEPHONY);
    }

    private void updateProximityWakeLock() {
        if ((callAudioActive || callProximityActive) && !speakerphoneEnabled) {
            acquireProximityWakeLock();
        } else {
            releaseProximityWakeLock(callAudioActive);
        }
    }

    private void acquireProximityWakeLock() {
        if (powerManager == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
            return;
        }

        if (!powerManager.isWakeLockLevelSupported(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK)) {
            return;
        }

        if (proximityWakeLock == null) {
            proximityWakeLock = powerManager.newWakeLock(
                PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK,
                "patronus:call_proximity");
            proximityWakeLock.setReferenceCounted(false);
        }

        if (!proximityWakeLock.isHeld()) {
            proximityWakeLock.acquire();
        }
    }

    private void releaseProximityWakeLock(boolean waitForNoProximity) {
        if (proximityWakeLock == null || !proximityWakeLock.isHeld()) {
            return;
        }

        try {
            if (waitForNoProximity && Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                proximityWakeLock.release(PowerManager.RELEASE_FLAG_WAIT_FOR_NO_PROXIMITY);
            } else {
                proximityWakeLock.release();
            }
        } catch (RuntimeException ignored) {
        }
    }

    private void requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return;
        }

        if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
            return;
        }

        requestPermissions(new String[] { android.Manifest.permission.POST_NOTIFICATIONS }, NOTIFICATION_PERMISSION_REQUEST_CODE);
    }

    public String saveFileToDownloads(String sourcePath, String displayName) {
        if (sourcePath == null || sourcePath.isEmpty()) {
            return "";
        }

        File sourceFile = new File(sourcePath);
        if (!sourceFile.exists() || !sourceFile.isFile()) {
            return "";
        }

        String safeName = (displayName != null && !displayName.isEmpty())
            ? displayName
            : sourceFile.getName();

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                return saveFileToScopedDownloads(sourceFile, safeName);
            }

            return saveFileToLegacyDownloads(sourceFile, safeName);
        } catch (IOException ignored) {
            return "";
        }
    }

    public boolean installApk(String apkPath) {
        if (apkPath == null || apkPath.isEmpty()) {
            return false;
        }

        File apkFile = new File(apkPath);
        if (!apkFile.exists() || !apkFile.isFile()) {
            return false;
        }

        Uri contentUri;
        try {
            contentUri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", apkFile);
        } catch (IllegalArgumentException exception) {
            Log.e(TAG, "Failed to create FileProvider URI for APK", exception);
            return false;
        }

        Intent intent = new Intent(Intent.ACTION_VIEW);
        intent.setDataAndType(contentUri, "application/vnd.android.package-archive");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);

        if (intent.resolveActivity(getPackageManager()) == null) {
            Log.e(TAG, "No activity found to install APK");
            return false;
        }

        runOnUiThread(() -> startActivity(intent));
        return true;
    }

    public boolean shareImage(String path, String text) {
        if (path == null || path.isEmpty()) {
            return false;
        }

        File file = new File(path);
        if (!file.exists() || !file.isFile()) {
            return false;
        }

        Uri contentUri;
        try {
            contentUri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", file);
        } catch (IllegalArgumentException exception) {
            Log.e(TAG, "Failed to create FileProvider URI for share", exception);
            return false;
        }

        Intent intent = new Intent(Intent.ACTION_SEND);
        intent.setType("image/png");
        intent.putExtra(Intent.EXTRA_STREAM, contentUri);
        if (text != null && !text.isEmpty()) {
            intent.putExtra(Intent.EXTRA_TEXT, text);
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

        Intent chooser = Intent.createChooser(intent, "Share QR");
        chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

        if (intent.resolveActivity(getPackageManager()) == null) {
            Log.e(TAG, "No activity found to share image");
            return false;
        }

        runOnUiThread(() -> startActivity(chooser));
        return true;
    }

    public void initializeFcmForServer(String serverKey, String clientConfigJson) {
        Log.d(TAG, "initializeFcmForServer serverKey=" + serverKey
                + " configBytes=" + (clientConfigJson != null ? clientConfigJson.length() : 0));
        try {
            ServerFirebaseManager.getInstance(this).initialize(serverKey, clientConfigJson);
        } catch (RuntimeException exception) {
            Log.e(TAG, "initializeFcmForServer failed serverKey=" + serverKey, exception);
            nativeOnFcmTokenError(serverKey, exception.getMessage());
        }
    }

    public void requestFcmTokenForServer(String serverKey) {
        Log.d(TAG, "requestFcmTokenForServer serverKey=" + serverKey);
        ServerFirebaseManager.getInstance(this).requestToken(serverKey, new ServerFirebaseManager.TokenCallback() {
            @Override
            public void onSuccess(String token) {
                Log.d(TAG, "requestFcmTokenForServer success serverKey=" + serverKey
                        + " tokenLen=" + token.length());
                nativeOnFcmTokenReady(serverKey, token);
            }

            @Override
            public void onError(String errorText) {
                Log.e(TAG, "requestFcmTokenForServer error serverKey=" + serverKey + " error=" + errorText);
                nativeOnFcmTokenError(serverKey, errorText);
            }
        });
    }

    public String getCachedFcmTokenForServer(String serverKey) {
        String token = ServerFirebaseManager.getInstance(this).getCachedToken(serverKey);
        Log.d(TAG, "getCachedFcmTokenForServer serverKey=" + serverKey
                + " tokenLen=" + (token != null ? token.length() : 0));
        return token;
    }

    public void clearFcmServerState(String serverKey) {
        Log.d(TAG, "clearFcmServerState serverKey=" + serverKey);
        ServerFirebaseManager.getInstance(this).clearServerState(serverKey);
    }

    private String saveFileToScopedDownloads(File sourceFile, String displayName) throws IOException {
        ContentValues values = new ContentValues();
        values.put(MediaStore.MediaColumns.DISPLAY_NAME, displayName);
        values.put(MediaStore.MediaColumns.MIME_TYPE, resolveMimeType(displayName));
        values.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
        values.put(MediaStore.MediaColumns.IS_PENDING, 1);

        Uri uri = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (uri == null) {
            return "";
        }

        boolean success = false;
        try (InputStream inputStream = new FileInputStream(sourceFile);
             OutputStream outputStream = getContentResolver().openOutputStream(uri, "w")) {
            if (outputStream == null) {
                return "";
            }

            copyStream(inputStream, outputStream);
            success = true;
        } finally {
            ContentValues updateValues = new ContentValues();
            updateValues.put(MediaStore.MediaColumns.IS_PENDING, 0);
            getContentResolver().update(uri, updateValues, null, null);

            if (!success) {
                getContentResolver().delete(uri, null, null);
            }
        }

        return success ? displayName : "";
    }

    private String saveFileToLegacyDownloads(File sourceFile, String displayName) throws IOException {
        File downloadsDirectory = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (downloadsDirectory == null) {
            return "";
        }

        if (!downloadsDirectory.exists() && !downloadsDirectory.mkdirs()) {
            return "";
        }

        File destinationFile = buildUniqueLegacyDownloadsFile(downloadsDirectory, displayName);
        try (InputStream inputStream = new FileInputStream(sourceFile);
             OutputStream outputStream = new java.io.FileOutputStream(destinationFile)) {
            copyStream(inputStream, outputStream);
        }

        return destinationFile.getAbsolutePath();
    }

    private File buildUniqueLegacyDownloadsFile(File downloadsDirectory, String displayName) {
        File candidate = new File(downloadsDirectory, displayName);
        if (!candidate.exists()) {
            return candidate;
        }

        String baseName = displayName;
        String extension = "";
        int dotIndex = displayName.lastIndexOf('.');
        if (dotIndex > 0) {
            baseName = displayName.substring(0, dotIndex);
            extension = displayName.substring(dotIndex);
        }

        int suffix = 1;
        while (candidate.exists()) {
            candidate = new File(downloadsDirectory, baseName + " (" + suffix + ")" + extension);
            suffix += 1;
        }
        return candidate;
    }

    private String resolveMimeType(String displayName) {
        String mimeType = URLConnection.guessContentTypeFromName(displayName);
        return mimeType != null ? mimeType : "application/octet-stream";
    }

    private void copyStream(InputStream inputStream, OutputStream outputStream) throws IOException {
        byte[] buffer = new byte[16 * 1024];
        int read;
        while ((read = inputStream.read(buffer)) != -1) {
            outputStream.write(buffer, 0, read);
        }
        outputStream.flush();
    }

    public void openFilePicker(String filter) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        String[] mimeTypes = resolveMimeTypes(filter);
        if (mimeTypes.length == 1) {
            intent.setType(mimeTypes[0]);
        } else {
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
        }
        startActivityForResult(intent, OPEN_FILE_REQUEST_CODE);
    }

    public void openFilesPicker(String filter) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        String[] mimeTypes = resolveMimeTypes(filter);
        if (mimeTypes.length == 1) {
            intent.setType(mimeTypes[0]);
        } else {
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
        }
        startActivityForResult(intent, OPEN_FILES_REQUEST_CODE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == OPEN_FILES_REQUEST_CODE) {
            handleMultiFileResult(resultCode, data);
            return;
        }

        if (requestCode != OPEN_FILE_REQUEST_CODE) {
            return;
        }

        if (resultCode != Activity.RESULT_OK || data == null || data.getData() == null) {
            nativeOnFilePickCancelled();
            return;
        }

        Uri uri = data.getData();
        String fileName = resolveDisplayName(uri);
        String filePath = copyPickedUriToCache(uri);
        if (filePath == null) {
            nativeOnFilePickCancelled();
            return;
        }

        nativeOnFilePicked(fileName, filePath);
    }

    private void handleMultiFileResult(int resultCode, Intent data) {
        if (resultCode != Activity.RESULT_OK || data == null) {
            nativeOnFilesPickCancelled();
            return;
        }

        java.util.ArrayList<Uri> uris = new java.util.ArrayList<>();
        ClipData clipData = data.getClipData();
        if (clipData != null) {
            for (int i = 0; i < clipData.getItemCount(); i++) {
                Uri itemUri = clipData.getItemAt(i).getUri();
                if (itemUri != null) {
                    uris.add(itemUri);
                }
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }

        if (uris.isEmpty()) {
            nativeOnFilesPickCancelled();
            return;
        }

        java.util.ArrayList<String> names = new java.util.ArrayList<>();
        java.util.ArrayList<String> paths = new java.util.ArrayList<>();
        for (Uri uri : uris) {
            String path = copyPickedUriToCache(uri);
            if (path == null) {
                continue;
            }
            names.add(resolveDisplayName(uri));
            paths.add(path);
        }

        if (names.isEmpty()) {
            nativeOnFilesPickCancelled();
            return;
        }

        nativeOnFilesPicked(names.toArray(new String[0]),
                            paths.toArray(new String[0]));
    }

    private String[] resolveMimeTypes(String filter) {
        if (filter == null) {
            return new String[] { "*/*" };
        }

        String lower = filter.toLowerCase();
        boolean wantsImage = lower.contains("image")
            || lower.contains("*.png")
            || lower.contains("*.jpg")
            || lower.contains("*.jpeg")
            || lower.contains("*.bmp")
            || lower.contains("*.gif")
            || lower.contains("*.webp");
        boolean wantsVideo = lower.contains("video")
            || lower.contains("*.mp4")
            || lower.contains("*.mkv")
            || lower.contains("*.webm")
            || lower.contains("*.avi")
            || lower.contains("*.mov")
            || lower.contains("*.m4v");
        boolean wantsAudio = lower.contains("audio") || lower.contains("voice");

        if (wantsImage && wantsVideo) {
            return new String[] { "image/*", "video/*" };
        }
        if (wantsImage) {
            return new String[] { "image/*" };
        }
        if (wantsAudio) {
            return new String[] { "audio/*" };
        }
        if (wantsVideo) {
            return new String[] { "video/*" };
        }
        return new String[] { "*/*" };
    }

    private String resolveDisplayName(Uri uri) {
        Cursor cursor = null;
        try {
            cursor = getContentResolver().query(uri, null, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (nameIndex >= 0) {
                    String displayName = cursor.getString(nameIndex);
                    if (displayName != null && !displayName.isEmpty()) {
                        return displayName;
                    }
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (cursor != null) {
                cursor.close();
            }
        }

        String lastPathSegment = uri.getLastPathSegment();
        return lastPathSegment != null ? lastPathSegment : "picked_file";
    }

    // --- SwiftKey composing fix (QTBUG-78857) ---
    // SwiftKey calls setComposingText() for each character; Qt doesn't update the display
    // when called with shorter text (backspace). Fix: before showSoftInput() we reflect
    // into Qt's class hierarchy to wrap QtEditText.m_inputConnection with
    // NoComposingInputConnection, then call restartInput(qtEditText) so Android recreates
    // the binder pointing to our wrapper. NoComposingInputConnection converts every
    // setComposingText() into deleteSurroundingText() + commitText() so Qt sees only
    // committed text and backspace always works.

    private static final String TAG_NC = "NoComposing";

    // Retry delays for IC wrap attempts (ms). 150ms lets the keyboard animation complete
    // before we call requestFocus/showSoftInput, avoiding layout jumps during animation.
    private static final int[] IC_WRAP_DELAYS = {0, 150, 350, 650};

    private void scheduleIcWrap(InputMethodManager imm, int attempt) {
        if (attempt >= IC_WRAP_DELAYS.length) {
            // All retries exhausted. Fall back: focus QtEditText explicitly so
            // onCreateInputConnection() is definitely triggered, then wrap the IC.
            Log.w(TAG_NC, "IC wrap: all retries exhausted, forcing via qtEditText");
            View qtEt = findQtEditTextView();
            if (qtEt != null) {
                qtEt.requestFocus();
                imm.showSoftInput(qtEt, InputMethodManager.SHOW_IMPLICIT);
                imm.restartInput(qtEt);
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
                    View w = wrapQtInputConnection();
                    if (w != null) {
                        try {
                            Class<?> cls = Class.forName("org.qtproject.qt.android.QtEditText");
                            Field optField = cls.getDeclaredField("m_optionsChanged");
                            optField.setAccessible(true);
                            optField.setBoolean(w, true);
                        } catch (Exception e) {
                            Log.w(TAG_NC, "Could not set m_optionsChanged (fallback): " + e.getMessage());
                        }
                        imm.restartInput(w);
                        Log.d(TAG_NC, "Wrapper activated (fallback after retries)");
                        reNotifyKeyboardHeight();
                    }
                }, 150);
            }
            return;
        }
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            View qtEt = findQtEditTextView();
            if (qtEt == null) {
                Log.d(TAG_NC, "qtEt null at " + IC_WRAP_DELAYS[attempt] + "ms, retrying...");
                scheduleIcWrap(imm, attempt + 1);
                return;
            }
            // On second show (QML activeFocus unchanged) Qt never calls lambda$2,
            // so mServedView is null and restartInput(wrapped) would be silently ignored.
            // requestFocus(qtEt) sets mNextServedView=qtEt (via Android framework internals),
            // then showSoftInput triggers checkFocus → mServedView=qtEt ✓.
            //
            // IMPORTANT: requestFocus triggers Qt's lambda$2 with RESET values (m_imeOptions=0)
            // synchronously on the Android main thread (runOnUiThread is a no-op when already
            // on main). We must call requestFocus BEFORE wrapQtInputConnection so that:
            // 1. lambda$2-wrong sets m_imeOptions=0 (synchronously, right here)
            // 2. We then overwrite m_imeOptions with correct value from mPendingImhHints
            // 3. m_optionsChanged=true + restartInput fills EditorInfo with correct imeOptions
            // 4. lambda$1-wrong (postDelayed 15ms from lambda$2-wrong) finds m_optionsChanged=false
            //    (restartInput reset it) → does NOT call restartInput again → no overwrite ✓
            if (!qtEt.isFocused()) {
                qtEt.requestFocus();
                imm.showSoftInput(qtEt, InputMethodManager.SHOW_IMPLICIT);
            }
            View wrapped = wrapQtInputConnection();
            if (wrapped != null) {
                // Qt resets m_imeOptions/m_inputType to defaults (0/1) when keyboard hides,
                // and only re-sets them via lambda$2 when QML activeFocus changes.
                // On repeated shows (activeFocus unchanged) the reset values stay.
                // We restore correct values from the QML hints passed at showKeyboard time,
                // then set m_optionsChanged=true so that onCreateInputConnection returns
                // our wrapper (existing IC) with a freshly filled, correct EditorInfo.
                try {
                    Class<?> cls = Class.forName("org.qtproject.qt.android.QtEditText");
                    int hints = mPendingImhHints;
                    Log.d(TAG_NC, "scheduleIcWrap: mPendingImhHints=0x" + Integer.toHexString(hints)
                            + " multiLine=" + ((hints & 0x400) != 0));
                    Field inputTypeField = cls.getDeclaredField("m_inputType");
                    Field imeOptField = cls.getDeclaredField("m_imeOptions");
                    inputTypeField.setAccessible(true);
                    imeOptField.setAccessible(true);
                    inputTypeField.setInt(wrapped, imhToAndroidInputType(hints));
                    imeOptField.setInt(wrapped, imhToAndroidImeOptions(hints));
                    int finalInputType = inputTypeField.getInt(wrapped);
                    int finalImeOptions = imeOptField.getInt(wrapped);
                    Log.d(TAG_NC, "Before restartInput: inputType=0x" + Integer.toHexString(finalInputType)
                            + " imeOptions=0x" + Integer.toHexString(finalImeOptions)
                            + " IME_FLAG_NO_ENTER_ACTION=" + ((finalImeOptions & 0x40000000) != 0));
                    Field optField = cls.getDeclaredField("m_optionsChanged");
                    optField.setAccessible(true);
                    optField.setBoolean(wrapped, true);
                } catch (Exception e) {
                    Log.w(TAG_NC, "Could not restore editor options: " + e.getMessage());
                }
                // API 33+: invalidateInput only triggers onCreateInputConnection
                // (lightweight EditorInfo refresh) without a full SwiftKey session reset
                // (onFinishInput → T9 hides → onStartInput → T9 reappears).
                // API <33: restartInput is the only option; T9 may briefly flicker.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    imm.invalidateInput(wrapped);
                    Log.d(TAG_NC, "Wrapper activated via invalidateInput at " + IC_WRAP_DELAYS[attempt] + "ms");
                } else {
                    imm.restartInput(wrapped);
                    Log.d(TAG_NC, "Wrapper activated via restartInput at " + IC_WRAP_DELAYS[attempt] + "ms");
                }
                // reNotifyKeyboardHeight() removed: sending 0→h caused a brief
                // bottomMargin=0 state that dropped the input field behind the keyboard.
                // Natural WindowInsets updates (736 on keyboard show, 944 when T9 bar
                // appears after restartInput) handle positioning correctly.
            } else {
                Log.d(TAG_NC, "IC null at " + IC_WRAP_DELAYS[attempt] + "ms, retrying...");
                scheduleIcWrap(imm, attempt + 1);
            }
        }, IC_WRAP_DELAYS[attempt]);
    }

    // Re-fires the current IME height to QML. Needed after the IC-init fallback: requesting
    // focus on qtEditText may have temporarily cleared QML's activeFocusItem, so the
    // onKeyboardHeightChanged auto-scroll handler returns early. Re-sending the height
    // forces the handler to re-run (via the 0→current cycle) with the correct T9-bar height.
    private void reNotifyKeyboardHeight() {
        View decorView = getWindow().getDecorView();
        int h = 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            android.view.WindowInsets insets = decorView.getRootWindowInsets();
            if (insets != null) {
                android.graphics.Insets imeInsets = insets.getInsets(android.view.WindowInsets.Type.ime());
                android.graphics.Insets navInsets = insets.getInsets(android.view.WindowInsets.Type.navigationBars());
                h = Math.max(0, imeInsets.bottom - navInsets.bottom);
            }
        } else {
            android.graphics.Rect frame = new android.graphics.Rect();
            decorView.getWindowVisibleDisplayFrame(frame);
            h = Math.max(0, decorView.getRootView().getHeight() - frame.bottom);
        }
        if (h > 0) {
            nativeOnKeyboardHeightChanged(0);
            nativeOnKeyboardHeightChanged(h);
            Log.d(TAG_NC, "Re-notified keyboard height=" + h + " to QML");
        }
    }

    // Traverses Qt's class hierarchy to return the current QtEditText View, or null.
    private View findQtEditTextView() {
        try {
            Class<?> qtActivityBaseClass = null;
            for (Class<?> c = PatronusActivity.class.getSuperclass(); c != null; c = c.getSuperclass()) {
                if (c.getSimpleName().equals("QtActivityBase")) { qtActivityBaseClass = c; break; }
            }
            if (qtActivityBaseClass == null) return null;

            Field delegateField = qtActivityBaseClass.getDeclaredField("m_delegate");
            delegateField.setAccessible(true);
            Object delegate = delegateField.get(this);
            if (delegate == null) return null;

            Object qtInputDelegate = null;
            outer:
            for (Class<?> c = delegate.getClass(); c != null && c != Object.class; c = c.getSuperclass()) {
                for (Field f : c.getDeclaredFields()) {
                    f.setAccessible(true);
                    try {
                        Object val = f.get(delegate);
                        if (val != null && val.getClass().getName().endsWith("QtInputDelegate")) {
                            qtInputDelegate = val;
                            break outer;
                        }
                    } catch (Exception ignored) {}
                }
            }
            if (qtInputDelegate == null) return null;

            Field etField = Class.forName("org.qtproject.qt.android.QtInputDelegate")
                    .getDeclaredField("m_currentEditText");
            etField.setAccessible(true);
            Object et = etField.get(qtInputDelegate);
            return (et instanceof View) ? (View) et : null;
        } catch (Exception e) {
            Log.e(TAG_NC, "findQtEditTextView failed", e);
            return null;
        }
    }

    // Wraps QtEditText.m_inputConnection with NoComposingInputConnection.
    // Returns the QtEditText View on success; null if m_inputConnection is null or not found.
    private View wrapQtInputConnection() {
        try {
            View qtEditTextView = findQtEditTextView();
            if (qtEditTextView == null) {
                Log.w(TAG_NC, "QtEditText view not found");
                return null;
            }

            Class<?> qtEditTextClass = Class.forName("org.qtproject.qt.android.QtEditText");
            Field icField = qtEditTextClass.getDeclaredField("m_inputConnection");
            icField.setAccessible(true);
            Object qtIc = icField.get(qtEditTextView);

            if (qtIc == null) {
                Log.w(TAG_NC, "m_inputConnection is null in QtEditText");
                return null;
            }
            if (qtIc instanceof NoComposingInputConnection) {
                Log.d(TAG_NC, "Already wrapped");
                return qtEditTextView;
            }
            if (!(qtIc instanceof InputConnection)) {
                Log.w(TAG_NC, "m_inputConnection is not InputConnection: " + qtIc.getClass().getName());
                return null;
            }

            // Log imeOptions/inputType so we can diagnose wrong EditorInfo (e.g. Enter hides keyboard)
            try {
                Field imeOptField = qtEditTextClass.getDeclaredField("m_imeOptions");
                imeOptField.setAccessible(true);
                Field inputTypeField = qtEditTextClass.getDeclaredField("m_inputType");
                inputTypeField.setAccessible(true);
                int imeOpts = imeOptField.getInt(qtEditTextView);
                int inputType = inputTypeField.getInt(qtEditTextView);
                Log.d(TAG_NC, "Wrap: m_imeOptions=0x" + Integer.toHexString(imeOpts)
                        + " m_inputType=0x" + Integer.toHexString(inputType)
                        + " IME_FLAG_NO_ENTER_ACTION=" + ((imeOpts & 0x40000000) != 0)
                        + " MULTI_LINE=" + ((inputType & 0x20000) != 0));
            } catch (Exception ex) {
                Log.w(TAG_NC, "Could not read imeOptions: " + ex.getMessage());
            }
            NoComposingInputConnection wrapper = new NoComposingInputConnection((InputConnection) qtIc);
            // Field.set() enforces the declared type (QtInputConnection) and rejects our
            // InputConnectionWrapper subclass. Use Unsafe.putObject() to bypass the type check —
            // ART does not re-validate object references on field reads, only on casts.
            Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
            Field unsafeField = unsafeClass.getDeclaredField("theUnsafe");
            unsafeField.setAccessible(true);
            Object unsafe = unsafeField.get(null);
            long offset = (long) unsafeClass
                    .getMethod("objectFieldOffset", Field.class)
                    .invoke(unsafe, icField);
            unsafeClass
                    .getMethod("putObject", Object.class, long.class, Object.class)
                    .invoke(unsafe, qtEditTextView, offset, wrapper);
            Log.d(TAG_NC, "*** Wrapped QtInputConnection in QtEditText.m_inputConnection ***");
            return qtEditTextView;
        } catch (Exception e) {
            Log.e(TAG_NC, "wrapQtInputConnection failed", e);
            return null;
        }
    }

    private static class NoComposingInputConnection extends InputConnectionWrapper {
        // Tracks how many chars we committed as "fake composing" so we can
        // delete them when setComposingText() is called with shorter text.
        private int mComposingLength = 0;

        NoComposingInputConnection(InputConnection target) {
            super(target, false);
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            String newText = text != null ? text.toString() : "";
            Log.d("NoComposing", "setComposingText '" + newText + "' cursor=" + newCursorPosition + " prevLen=" + mComposingLength);
            if (mComposingLength > 0) {
                super.deleteSurroundingText(mComposingLength, 0);
            }
            mComposingLength = newText.length();
            boolean result = true;
            if (!newText.isEmpty()) {
                result = super.commitText(newText, newCursorPosition);
            }
            super.finishComposingText();
            mComposingLength = 0;
            Log.d("NoComposing", "setComposingText done result=" + result);
            return result;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            Log.d("NoComposing", "commitText '" + text + "' prevLen=" + mComposingLength);
            if (mComposingLength > 0) {
                super.deleteSurroundingText(mComposingLength, 0);
                mComposingLength = 0;
            }
            return super.commitText(text, newCursorPosition);
        }

        @Override
        public boolean finishComposingText() {
            Log.d("NoComposing", "finishComposingText prevLen=" + mComposingLength);
            mComposingLength = 0;
            return super.finishComposingText();
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            Log.d("NoComposing", "deleteSurroundingText before=" + beforeLength + " after=" + afterLength + " prevLen=" + mComposingLength);
            if (mComposingLength > 0 && beforeLength > 0) {
                mComposingLength = Math.max(0, mComposingLength - beforeLength);
            }
            return super.deleteSurroundingText(beforeLength, afterLength);
        }

        @Override
        public boolean setComposingRegion(int start, int end) {
            Log.d("NoComposing", "setComposingRegion " + start + "-" + end + " (suppressed)");
            return true;
        }

        @Override
        public boolean performEditorAction(int actionCode) {
            Log.d("NoComposing", "performEditorAction actionCode=" + actionCode
                    + " (DONE=6, SEND=4, GO=2, SEARCH=3, NONE=1, UNSPECIFIED=0)");
            return super.performEditorAction(actionCode);
        }
    }
    // --- end SwiftKey composing fix ---

    // Streams the picked content straight into a private cache file instead of
    // buffering the whole thing in a Java byte[] and shipping it across JNI as
    // one array — a multi-GB-heap-unfriendly video (or several files picked at
    // once for an album) could blow past the app's heap growth limit and throw
    // an uncaught OutOfMemoryError (ByteArrayOutputStream.grow), crashing the
    // whole process mid-pick. Native reads the result back off disk instead.
    private String copyPickedUriToCache(Uri uri) {
        File cacheDir = new File(getCacheDir(), "picked");
        if (!cacheDir.exists() && !cacheDir.mkdirs()) {
            return null;
        }

        File targetFile;
        try {
            targetFile = File.createTempFile("picked_", ".tmp", cacheDir);
        } catch (IOException exception) {
            return null;
        }

        try (InputStream inputStream = getContentResolver().openInputStream(uri);
             OutputStream outputStream = new FileOutputStream(targetFile)) {
            if (inputStream == null) {
                targetFile.delete();
                return null;
            }

            copyStream(inputStream, outputStream);
            return targetFile.getAbsolutePath();
        } catch (IOException exception) {
            targetFile.delete();
            return null;
        }
    }
}