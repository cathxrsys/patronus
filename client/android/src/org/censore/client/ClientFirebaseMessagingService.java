package org.patronus.client;

import android.util.Log;

import com.google.firebase.messaging.FirebaseMessagingService;
import com.google.firebase.messaging.RemoteMessage;

public class ClientFirebaseMessagingService extends FirebaseMessagingService {
    private static final String TAG = "PatronusFCM";

    @Override
    public void onNewToken(String token) {
        super.onNewToken(token);
        Log.d(TAG, "onNewToken tokenLen=" + token.length());
        ServerFirebaseManager.getInstance(this).refreshAllTokens(true);
    }

    @Override
    public void onMessageReceived(RemoteMessage remoteMessage) {
        super.onMessageReceived(remoteMessage);

        Log.d(TAG, "onMessageReceived from=" + remoteMessage.getFrom()
                + " dataSize=" + remoteMessage.getData().size()
                + " hasNotification=" + (remoteMessage.getNotification() != null));

        // Data-only call push: deliberately carries no sensitive data, only
        // type=call. Show a generic incoming-call notification with a full-screen
        // intent; that launches the app (over the lockscreen if needed), which
        // then comes online, receives the encrypted call_request and rings. The
        // notification is updated with the caller's name once that arrives.
        String type = remoteMessage.getData().get("type");
        if ("call".equals(type)) {
            Log.d(TAG, "onMessageReceived incoming call push");
            NotificationHelper.showIncomingCallNotification(this);
            return;
        }
        if ("call_cancel".equals(type)) {
            // Caller hung up before we answered: stop ringing and dismiss the
            // incoming-call notification.
            Log.d(TAG, "onMessageReceived call cancelled push");
            NotificationHelper.cancelIncomingCallNotification(this);
            return;
        }

        String serverKey = remoteMessage.getData().get("serverKey");
        if (serverKey == null || serverKey.isEmpty()) {
            serverKey = remoteMessage.getData().get("server_key");
        }
        if ((serverKey == null || serverKey.isEmpty()) && remoteMessage.getFrom() != null) {
            serverKey = ServerFirebaseManager.getInstance(this).resolveServerKey(remoteMessage.getFrom());
        }

        Log.d(TAG, "onMessageReceived resolved serverKey=" + serverKey);

        String body = remoteMessage.getData().get("body");
        if (body == null || body.isEmpty()) {
            if (remoteMessage.getNotification() != null && remoteMessage.getNotification().getBody() != null) {
                body = remoteMessage.getNotification().getBody();
            } else {
                body = "New message";
            }
        }

        String title = remoteMessage.getData().get("title");
        if (title == null || title.isEmpty()) {
            title = remoteMessage.getNotification() != null && remoteMessage.getNotification().getTitle() != null
                ? remoteMessage.getNotification().getTitle()
                : "patronus";
        }

        Log.d(TAG, "onMessageReceived showing notification title=" + title
                + " bodyLen=" + body.length());

        NotificationHelper.showSystemNotification(this, title, body);
    }
}