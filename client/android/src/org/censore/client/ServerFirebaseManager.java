package org.patronus.client;

import android.content.Context;
import android.content.Intent;
import android.util.Log;

import com.google.android.gms.tasks.OnCompleteListener;
import com.google.android.gms.tasks.Task;
import com.google.firebase.FirebaseApp;
import com.google.firebase.FirebaseOptions;
import com.google.firebase.messaging.FirebaseMessaging;

import org.json.JSONException;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.List;

final class ServerFirebaseManager {
    private static final String TAG = "PatronusFCM";

    interface TokenCallback {
        void onSuccess(String token);
        void onError(String errorText);
    }

    static final String ACTION_FCM_TOKEN_UPDATED = "org.patronus.client.FCM_TOKEN_UPDATED";
    static final String EXTRA_SERVER_KEY = "serverKey";
    static final String EXTRA_TOKEN = "token";
    static final String EXTRA_ERROR = "error";

    private static ServerFirebaseManager instance;

    private final Context applicationContext;
    private final ServerFcmStore store;
    private final ServerFcmConfigParser configParser = new ServerFcmConfigParser();

    private ServerFirebaseManager(Context context) {
        applicationContext = context.getApplicationContext();
        store = new ServerFcmStore(applicationContext);
    }

    static synchronized ServerFirebaseManager getInstance(Context context) {
        if (instance == null) {
            instance = new ServerFirebaseManager(context);
        }
        return instance;
    }

    void initialize(String serverKey, String configJson) {
        Log.d(TAG, "initialize serverKey=" + serverKey
            + " configBytes=" + (configJson != null ? configJson.length() : 0));
        ServerFcmConfigParser.ParsedConfig parsedConfig = parseConfig(configJson);
        String appName = stableAppName(serverKey);
        String newConfigHash = ServerFcmStore.stableId(configJson);
        String oldConfigHash = store.getConfigHash(serverKey);
        boolean configChanged = !newConfigHash.equals(oldConfigHash);
        Log.d(TAG, "initialize parsed config serverKey=" + serverKey
            + " projectNumber=" + parsedConfig.projectNumber
            + " configChanged=" + configChanged);

        if (configChanged) {
            Log.d(TAG, "initialize clearing previous app/token state serverKey=" + serverKey);
            deleteExistingApp(appName);
            store.setCachedToken(serverKey, "");
            store.setTokenDirty(serverKey, true);
            store.setLastRegistrationStatus(serverKey, "pending");
        }

        FirebaseApp existingApp = findApp(appName);
        if (existingApp == null) {
            Log.d(TAG, "initialize creating FirebaseApp appName=" + appName);
            FirebaseApp.initializeApp(applicationContext, parsedConfig.options, appName);
            store.saveConfig(serverKey, configJson, newConfigHash, appName, parsedConfig.projectNumber);
            return;
        }

        Log.d(TAG, "initialize reusing existing FirebaseApp appName=" + appName);
        store.saveConfig(serverKey, configJson, newConfigHash, appName, parsedConfig.projectNumber);
    }

    void requestToken(final String serverKey, final TokenCallback callback) {
        Log.d(TAG, "requestToken serverKey=" + serverKey);
        requestTokenInternal(serverKey, callback, false);
    }

    String getCachedToken(String serverKey) {
        return store.getCachedToken(serverKey);
    }

    void clearServerState(String serverKey) {
        Log.d(TAG, "clearServerState serverKey=" + serverKey);
        deleteExistingApp(stableAppName(serverKey));
        store.clearServer(serverKey);
    }

    void refreshAllTokens(boolean notify) {
        Log.d(TAG, "refreshAllTokens notify=" + notify
                + " count=" + store.getKnownServerKeys().size());
        for (String serverKey : store.getKnownServerKeys()) {
            requestTokenInternal(serverKey, null, notify);
        }
    }

    String resolveServerKey(String senderId) {
        String serverKey = store.findServerKeyBySenderId(senderId);
        Log.d(TAG, "resolveServerKey senderId=" + senderId + " serverKey=" + serverKey);
        return serverKey;
    }

    private void requestTokenInternal(final String serverKey, final TokenCallback callback, final boolean notify) {
        FirebaseApp app = ensureApp(serverKey);
        if (app == null) {
            Log.e(TAG, "requestTokenInternal no FirebaseApp serverKey=" + serverKey);
            notifyError(serverKey, callback, "Firebase app is not initialized for server");
            return;
        }

        Log.d(TAG, "requestTokenInternal using FirebaseApp serverKey=" + serverKey
                + " appName=" + app.getName());

        FirebaseMessaging messaging = firebaseMessagingForApp(app);
        if (messaging == null) {
            Log.e(TAG, "requestTokenInternal FirebaseMessaging unavailable serverKey=" + serverKey);
            notifyError(serverKey, callback, "Unable to access FirebaseMessaging for app instance");
            return;
        }

        Log.d(TAG, "requestTokenInternal requesting token serverKey=" + serverKey);

        messaging.getToken().addOnCompleteListener(new OnCompleteListener<String>() {
            @Override
            public void onComplete(Task<String> task) {
                if (!task.isSuccessful()) {
                    String errorText = task.getException() != null
                        ? task.getException().getMessage()
                        : "Failed to obtain FCM token";
                    Log.e(TAG, "requestTokenInternal failed serverKey=" + serverKey + " error=" + errorText,
                            task.getException());
                    notifyError(serverKey, callback, errorText);
                    return;
                }

                String token = task.getResult();
                Log.d(TAG, "requestTokenInternal success serverKey=" + serverKey
                        + " tokenLen=" + (token != null ? token.length() : 0)
                        + " notify=" + notify);
                if (token == null || token.isEmpty()) {
                    notifyError(serverKey, callback, "Received empty FCM token");
                    return;
                }

                store.setCachedToken(serverKey, token);
                store.setTokenDirty(serverKey, true);
                store.setLastRegistrationStatus(serverKey, "ready");
                if (notify) {
                    broadcastTokenUpdate(serverKey, token, null);
                }
                if (callback != null) {
                    callback.onSuccess(token);
                }
            }
        });
    }

    private FirebaseApp ensureApp(String serverKey) {
        String appName = store.getAppName(serverKey);
        if (appName == null || appName.isEmpty()) {
            appName = stableAppName(serverKey);
        }

        FirebaseApp app = findApp(appName);
        if (app != null) {
            Log.d(TAG, "ensureApp found existing app serverKey=" + serverKey + " appName=" + appName);
            return app;
        }

        String configJson = store.getConfigJson(serverKey);
        if (configJson == null || configJson.isEmpty()) {
            Log.w(TAG, "ensureApp missing config serverKey=" + serverKey);
            return null;
        }

        ServerFcmConfigParser.ParsedConfig parsedConfig;
        try {
            parsedConfig = parseConfig(configJson);
        } catch (RuntimeException exception) {
            Log.e(TAG, "ensureApp parse failure serverKey=" + serverKey, exception);
            return null;
        }

        Log.d(TAG, "ensureApp creating app serverKey=" + serverKey + " appName=" + appName);
        return FirebaseApp.initializeApp(applicationContext, parsedConfig.options, appName);
    }

    private FirebaseMessaging firebaseMessagingForApp(FirebaseApp app) {
        try {
            Method method = FirebaseMessaging.class.getDeclaredMethod("getInstance", FirebaseApp.class);
            method.setAccessible(true);
            Log.d(TAG, "firebaseMessagingForApp reflection success appName=" + app.getName());
            return (FirebaseMessaging) method.invoke(null, app);
        } catch (NoSuchMethodException exception) {
            Log.e(TAG, "firebaseMessagingForApp missing method appName=" + app.getName(), exception);
            return null;
        } catch (IllegalAccessException exception) {
            Log.e(TAG, "firebaseMessagingForApp illegal access appName=" + app.getName(), exception);
            return null;
        } catch (InvocationTargetException exception) {
            Log.e(TAG, "firebaseMessagingForApp invocation failed appName=" + app.getName(), exception);
            return null;
        }
    }

    private ServerFcmConfigParser.ParsedConfig parseConfig(String configJson) {
        try {
            return configParser.parse(applicationContext, configJson);
        } catch (JSONException exception) {
            throw new IllegalStateException("Invalid Firebase client config: " + exception.getMessage(), exception);
        }
    }

    private void notifyError(String serverKey, TokenCallback callback, String errorText) {
        Log.e(TAG, "notifyError serverKey=" + serverKey + " error=" + errorText);
        store.setLastRegistrationStatus(serverKey, "error");
        if (callback != null) {
            callback.onError(errorText);
        }
        broadcastTokenUpdate(serverKey, null, errorText);
    }

    private void broadcastTokenUpdate(String serverKey, String token, String errorText) {
        Log.d(TAG, "broadcastTokenUpdate serverKey=" + serverKey
            + " tokenLen=" + (token != null ? token.length() : 0)
            + " hasError=" + (errorText != null && !errorText.isEmpty()));
        Intent intent = new Intent(ACTION_FCM_TOKEN_UPDATED);
        intent.setPackage(applicationContext.getPackageName());
        intent.putExtra(EXTRA_SERVER_KEY, serverKey);
        if (token != null) {
            intent.putExtra(EXTRA_TOKEN, token);
        }
        if (errorText != null) {
            intent.putExtra(EXTRA_ERROR, errorText);
        }
        applicationContext.sendBroadcast(intent);
    }

    private FirebaseApp findApp(String appName) {
        List<FirebaseApp> apps = FirebaseApp.getApps(applicationContext);
        for (FirebaseApp app : apps) {
            if (appName.equals(app.getName())) {
                return app;
            }
        }
        return null;
    }

    private void deleteExistingApp(String appName) {
        FirebaseApp app = findApp(appName);
        if (app != null) {
            Log.d(TAG, "deleteExistingApp appName=" + appName);
            app.delete();
        }
    }

    private String stableAppName(String serverKey) {
        return "fcm_" + ServerFcmStore.stableId(serverKey).substring(0, 16);
    }
}