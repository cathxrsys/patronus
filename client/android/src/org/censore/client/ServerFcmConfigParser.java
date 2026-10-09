package org.patronus.client;

import android.content.Context;

import com.google.firebase.FirebaseOptions;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

final class ServerFcmConfigParser {
    static final class ParsedConfig {
        final FirebaseOptions options;
        final String projectNumber;

        ParsedConfig(FirebaseOptions options, String projectNumber) {
            this.options = options;
            this.projectNumber = projectNumber;
        }
    }

    ParsedConfig parse(Context context, String configJson) throws JSONException {
        JSONObject root = new JSONObject(configJson);
        JSONObject projectInfo = root.getJSONObject("project_info");
        String projectId = projectInfo.getString("project_id");
        String projectNumber = projectInfo.getString("project_number");

        JSONArray clients = root.getJSONArray("client");
        JSONObject selectedClient = null;
        String packageName = context.getPackageName();
        for (int index = 0; index < clients.length(); ++index) {
            JSONObject client = clients.getJSONObject(index);
            JSONObject clientInfo = client.optJSONObject("client_info");
            if (clientInfo == null) {
                continue;
            }
            JSONObject androidClientInfo = clientInfo.optJSONObject("android_client_info");
            if (androidClientInfo == null) {
                continue;
            }
            if (packageName.equals(androidClientInfo.optString("package_name"))) {
                selectedClient = client;
                break;
            }
        }

        if (selectedClient == null) {
            throw new JSONException("No matching client entry for package " + packageName);
        }

        JSONObject clientInfo = selectedClient.getJSONObject("client_info");
        JSONArray apiKeys = selectedClient.getJSONArray("api_key");
        if (apiKeys.length() == 0) {
            throw new JSONException("No api_key entry in Firebase config");
        }

        String applicationId = clientInfo.getString("mobilesdk_app_id");
        String apiKey = apiKeys.getJSONObject(0).getString("current_key");
        FirebaseOptions options = new FirebaseOptions.Builder()
            .setProjectId(projectId)
            .setApplicationId(applicationId)
            .setApiKey(apiKey)
            .setGcmSenderId(projectNumber)
            .build();
        return new ParsedConfig(options, projectNumber);
    }
}