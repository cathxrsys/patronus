package org.patronus.client;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.service.notification.StatusBarNotification;

final class NotificationHelper {
    static final String APP_NOTIFICATION_CHANNEL_ID = "app_notifications_v4";
    // v2: the v1 channel was created with a ringtone sound + vibration baked in,
    // which is immutable; we now drive ringing ourselves (IncomingCallRinger) and
    // need a fresh silent channel.
    static final String INCOMING_CALL_CHANNEL_ID = "incoming_calls_v2";

    // Fixed id so the generic notification posted from FCM can later be replaced
    // (with the caller's name) or cancelled once the call is shown/ended.
    static final int INCOMING_CALL_NOTIFICATION_ID = 0xCA11;

    // Fixed id shared by every message notification so they never pile up: a new
    // one replaces the previous (and re-alerts) instead of stacking a fresh entry
    // for each message.
    static final int APP_MESSAGE_NOTIFICATION_ID = 0x4D5347;  // 'MSG'

    private static final long[] VIBRATION_PATTERN = { 0, 180, 80, 180 };

    // True while an incoming-call notification (id INCOMING_CALL_NOTIFICATION_ID)
    // is live. The FCM service sets it; the in-app call flow consults it so that
    // a notification is only ever updated, never spuriously created for a live
    // (foreground) call that never had an FCM-posted notification to begin with.
    private static volatile boolean incomingCallActive = false;

    private NotificationHelper() {
    }

    static boolean isIncomingCallActive() {
        return incomingCallActive;
    }

    // The status bar/tray only ever renders the alpha channel of the small
    // icon (everything opaque becomes a solid white/tinted blob), so the
    // full-color launcher icon shows up as a plain white square there. The
    // adaptive icon set already ships a monochrome silhouette (transparent
    // background, opaque glyph) meant for exactly this; fall back to the
    // launcher icon if it's ever missing. Resolved by name (no compile-time
    // R dependency), matching IncomingCallRinger's approach.
    private static int smallIconResId(Context context) {
        int resId = context.getResources().getIdentifier(
            "ic_launcher_monochrome", "mipmap", context.getPackageName());
        return resId != 0 ? resId : context.getApplicationInfo().icon;
    }

    static void showSystemNotification(Context context, String title, String message) {
        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager == null) {
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
            && context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            return;
        }

        ensureNotificationChannel(manager);

        Intent launchIntent = new Intent(context, PatronusActivity.class);
        launchIntent.setFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);

        int pendingIntentFlags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            pendingIntentFlags |= PendingIntent.FLAG_IMMUTABLE;
        }

        PendingIntent pendingIntent = PendingIntent.getActivity(context, 0, launchIntent, pendingIntentFlags);

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
            ? new Notification.Builder(context, APP_NOTIFICATION_CHANNEL_ID)
            : new Notification.Builder(context);

        builder
            .setSmallIcon(smallIconResId(context))
            .setContentTitle(title)
            .setContentText(message)
            .setAutoCancel(true)
            .setContentIntent(pendingIntent)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setCategory(Notification.CATEGORY_MESSAGE);

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            builder.setPriority(Notification.PRIORITY_MAX);
        }

        // Reuse a single id so message notifications collapse into one entry
        // rather than accumulating; posting the same id re-alerts the user.
        manager.notify(APP_MESSAGE_NOTIFICATION_ID, builder.build());
    }

    // Posted from the FCM service when a call_request push arrives while the app
    // is offline. It carries no caller info (the push has none) — just a generic
    // "Incoming call". A full-screen intent makes Android launch the app over the
    // lockscreen (or show a heads-up banner when the device is in use), so the
    // app can come online, receive the call_request and ring.
    static void showIncomingCallNotification(Context context) {
        // Fresh call: drop any stale action queued from a previous notification.
        PatronusActivity.clearPendingCallAction();

        Notification notification = buildIncomingCallNotification(
            context, "Incoming call", "Tap to answer", false);
        if (notification == null) {
            return;
        }
        incomingCallActive = true;
        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager != null) {
            manager.notify(INCOMING_CALL_NOTIFICATION_ID, notification);
        }

        // The channel is silent; we drive the (looping) ringtone + vibration here.
        // This is the single ringer for an FCM-surfaced call in every state
        // (locked, screen off, or heads-up while unlocked). When the app puts the
        // call on screen it suppresses its own in-app ringtone (see CallManager),
        // so there is never a double ring; this stops only on accept/decline/cancel.
        IncomingCallRinger.start(context);
    }

    // Called once the app has decrypted the call_request and resolved who is
    // calling. Replaces the generic notification in place with the caller's name.
    // alertOnce is set so the update does not re-ring or re-vibrate. No-op if no
    // incoming-call notification is currently showing (e.g. a live foreground
    // call that never went through FCM).
    static void updateIncomingCallNotification(Context context, String callerName) {
        if (!incomingCallActive) {
            return;
        }
        // Ringing keeps going (IncomingCallRinger is the single ringer); we only
        // relabel the notification with the resolved caller name here.
        String title = (callerName == null || callerName.isEmpty()) ? "Incoming call" : callerName;
        Notification notification = buildIncomingCallNotification(
            context, title, "Incoming call", true);
        if (notification == null) {
            return;
        }
        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager != null) {
            manager.notify(INCOMING_CALL_NOTIFICATION_ID, notification);
        }
    }

    // Called when the app is brought to the foreground: sweep away the message
    // notifications the user left piled up in the tray (each is posted with a
    // unique System.currentTimeMillis() id, so they accumulate until dismissed).
    // A live incoming-call notification is deliberately left alone -- its own
    // flow (accept/decline/cancel) owns and cancels it.
    static void clearMessageNotifications(Context context) {
        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager == null) {
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            for (StatusBarNotification active : manager.getActiveNotifications()) {
                if (active.getId() != INCOMING_CALL_NOTIFICATION_ID) {
                    manager.cancel(active.getTag(), active.getId());
                }
            }
        } else if (!incomingCallActive) {
            manager.cancelAll();
        }
    }

    static void cancelIncomingCallNotification(Context context) {
        incomingCallActive = false;
        IncomingCallRinger.stop();
        PatronusActivity.clearPendingCallAction();
        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager != null) {
            manager.cancel(INCOMING_CALL_NOTIFICATION_ID);
        }
    }

    private static Notification buildIncomingCallNotification(Context context, String title, String body, boolean alertOnce) {
        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager == null) {
            return null;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
            && context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            return null;
        }

        ensureIncomingCallChannel(manager);

        PendingIntent contentIntent = callActionIntent(context, null, 1);

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
            ? new Notification.Builder(context, INCOMING_CALL_CHANNEL_ID)
            : new Notification.Builder(context);

        builder
            .setSmallIcon(smallIconResId(context))
            .setContentTitle(title)
            .setContentText(body != null ? body : title)
            .setOngoing(true)
            .setAutoCancel(false)
            .setOnlyAlertOnce(alertOnce)
            .setContentIntent(contentIntent)
            // When the device is locked/asleep this launches the app full-screen
            // like a phone call; otherwise it surfaces as a heads-up banner.
            .setFullScreenIntent(contentIntent, true)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setCategory(Notification.CATEGORY_CALL)
            // Each opens the app and auto-performs the action once it is online.
            .addAction(0, "Decline", callActionIntent(context, PatronusActivity.CALL_ACTION_DECLINE, 3))
            .addAction(0, "Accept", callActionIntent(context, PatronusActivity.CALL_ACTION_ACCEPT, 2));

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            builder.setPriority(Notification.PRIORITY_MAX);
        }

        return builder.build();
    }

    // Builds a PendingIntent that launches the activity (over the lockscreen) and,
    // when action is non-null, tells it which call action to auto-perform once the
    // call is on screen. requestCode keeps the three intents distinct.
    private static PendingIntent callActionIntent(Context context, String action, int requestCode) {
        Intent intent = new Intent(context, PatronusActivity.class);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.putExtra(PatronusActivity.EXTRA_INCOMING_CALL, true);
        if (action != null) {
            intent.putExtra(PatronusActivity.EXTRA_CALL_ACTION, action);
        }

        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }
        return PendingIntent.getActivity(context, requestCode, intent, flags);
    }

    private static void ensureIncomingCallChannel(NotificationManager manager) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        // Drop the old sound-baked channel from earlier builds (immutable once
        // created, so we cannot silence it in place).
        manager.deleteNotificationChannel("incoming_calls_v1");
        if (manager.getNotificationChannel(INCOMING_CALL_CHANNEL_ID) != null) {
            return;
        }

        // High importance so the notification heads-up / goes full-screen, but
        // silent with no vibration: ringing is driven by IncomingCallRinger (so it
        // can loop) or by the app itself once it launches.
        NotificationChannel channel = new NotificationChannel(
            INCOMING_CALL_CHANNEL_ID,
            "Incoming calls",
            NotificationManager.IMPORTANCE_HIGH);
        channel.setDescription("Incoming voice calls");
        channel.enableLights(true);
        channel.enableVibration(false);
        channel.setSound(null, null);
        channel.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
        manager.createNotificationChannel(channel);
    }

    static void ensureNotificationChannel(NotificationManager manager) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }

        manager.deleteNotificationChannel("app_notifications_v1");
        manager.deleteNotificationChannel("app_notifications_v2");
        manager.deleteNotificationChannel("app_notifications_v3");

        if (manager.getNotificationChannel(APP_NOTIFICATION_CHANNEL_ID) != null) {
            return;
        }

        NotificationChannel channel = new NotificationChannel(
            APP_NOTIFICATION_CHANNEL_ID,
            "patronus",
            NotificationManager.IMPORTANCE_MAX);
        channel.setDescription("Application notifications");
        channel.enableLights(true);
        channel.enableVibration(true);
        channel.setVibrationPattern(VIBRATION_PATTERN);
        channel.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
        manager.createNotificationChannel(channel);
    }
}
