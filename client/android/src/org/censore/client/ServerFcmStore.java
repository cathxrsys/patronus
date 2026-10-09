package org.patronus.client;

import android.content.Context;
import android.content.SharedPreferences;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

final class ServerFcmStore {
    private static final String PREFS_NAME = "server_fcm_store";
    private static final String KEY_SERVER_SET = "servers";

    private final SharedPreferences preferences;

    ServerFcmStore(Context context) {
        preferences = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    String getCachedToken(String serverKey) {
        return preferences.getString(key(serverKey, "token"), "");
    }

    void setCachedToken(String serverKey, String token) {
        trackServer(serverKey);
        preferences.edit()
            .putString(key(serverKey, "token"), token)
            .apply();
    }

    String getConfigJson(String serverKey) {
        return preferences.getString(key(serverKey, "config_json"), "");
    }

    String getConfigHash(String serverKey) {
        return preferences.getString(key(serverKey, "config_hash"), "");
    }

    String getAppName(String serverKey) {
        return preferences.getString(key(serverKey, "app_name"), "");
    }

    String getSenderId(String serverKey) {
        return preferences.getString(key(serverKey, "sender_id"), "");
    }

    void saveConfig(String serverKey, String configJson, String configHash, String appName, String senderId) {
        trackServer(serverKey);
        preferences.edit()
            .putString(key(serverKey, "config_json"), configJson)
            .putString(key(serverKey, "config_hash"), configHash)
            .putString(key(serverKey, "app_name"), appName)
            .putString(key(serverKey, "sender_id"), senderId)
            .apply();
    }

    void setTokenDirty(String serverKey, boolean dirty) {
        trackServer(serverKey);
        preferences.edit()
            .putBoolean(key(serverKey, "dirty"), dirty)
            .apply();
    }

    boolean isTokenDirty(String serverKey) {
        return preferences.getBoolean(key(serverKey, "dirty"), false);
    }

    void setLastRegistrationStatus(String serverKey, String status) {
        trackServer(serverKey);
        preferences.edit()
            .putString(key(serverKey, "registration_status"), status)
            .apply();
    }

    void clearServer(String serverKey) {
        Set<String> servers = new HashSet<>(getKnownServerKeys());
        servers.remove(serverKey);
        preferences.edit()
            .remove(key(serverKey, "token"))
            .remove(key(serverKey, "config_json"))
            .remove(key(serverKey, "config_hash"))
            .remove(key(serverKey, "app_name"))
            .remove(key(serverKey, "sender_id"))
            .remove(key(serverKey, "dirty"))
            .remove(key(serverKey, "registration_status"))
            .putStringSet(KEY_SERVER_SET, servers)
            .apply();
    }

    Set<String> getKnownServerKeys() {
        Set<String> value = preferences.getStringSet(KEY_SERVER_SET, Collections.<String>emptySet());
        return value != null ? new HashSet<>(value) : new HashSet<String>();
    }

    String findServerKeyBySenderId(String senderId) {
        if (senderId == null || senderId.isEmpty()) {
            return "";
        }

        for (String serverKey : getKnownServerKeys()) {
            if (senderId.equals(getSenderId(serverKey))) {
                return serverKey;
            }
        }
        return "";
    }

    static String stableId(String rawValue) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] hash = digest.digest(rawValue.getBytes());
            StringBuilder builder = new StringBuilder(hash.length * 2);
            for (byte value : hash) {
                builder.append(String.format("%02x", value));
            }
            return builder.toString();
        } catch (NoSuchAlgorithmException exception) {
            return Integer.toHexString(rawValue.hashCode());
        }
    }

    private void trackServer(String serverKey) {
        Set<String> servers = new HashSet<>(getKnownServerKeys());
        if (servers.add(serverKey)) {
            preferences.edit().putStringSet(KEY_SERVER_SET, servers).apply();
        }
    }

    private String key(String serverKey, String suffix) {
        return "fcm." + stableId(serverKey) + "." + suffix;
    }
}