#include "DeviceNetwork.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

namespace {
constexpr uint32_t RECONNECT_INTERVAL_MS = 15000;
constexpr uint32_t REGISTRATION_RETRY_MS = 60000;
constexpr time_t VALID_TIME_EPOCH = 1700000000;

String jsonEscape(const String& value) {
  String escaped;
  const char hex[] = "0123456789abcdef";

  for (size_t index = 0; index < value.length(); index++) {
    const uint8_t character = static_cast<uint8_t>(value[index]);
    if (character == '"' || character == '\\') {
      escaped += '\\';
      escaped += static_cast<char>(character);
    } else if (character == '\n') {
      escaped += "\\n";
    } else if (character == '\r') {
      escaped += "\\r";
    } else if (character == '\t') {
      escaped += "\\t";
    } else if (character < 0x20) {
      escaped += "\\u00";
      escaped += hex[character >> 4];
      escaped += hex[character & 0x0f];
    } else {
      escaped += static_cast<char>(character);
    }
  }

  return escaped;
}
}

bool DeviceNetwork::begin(const DeviceNetworkConfig& config) {
  config_ = config;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);

  if (config_.wifiCredentials == nullptr || config_.wifiCredentialCount == 0) {
    return false;
  }

  size_t addedNetworks = 0;
  for (size_t index = 0; index < config_.wifiCredentialCount; index++) {
    const WifiCredential& credential = config_.wifiCredentials[index];
    if (credential.ssid == nullptr || credential.ssid[0] == '\0') {
      continue;
    }

    if (wifiMulti_.addAP(credential.ssid, credential.password)) {
      addedNetworks++;
    }
  }

  if (addedNetworks == 0) {
    return false;
  }

  lastReconnectAttempt_ = millis();
  return wifiMulti_.run(5000) == WL_CONNECTED;
}

void DeviceNetwork::loop() {
  const uint32_t now = millis();
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (!connected) {
    if (wasConnected_) {
      disconnectionEventPending_ = true;
    }
    wasConnected_ = false;
    if (now - lastReconnectAttempt_ >= RECONNECT_INTERVAL_MS) {
      lastReconnectAttempt_ = now;
      wifiMulti_.run(5000);
    }
    return;
  }

  if (!wasConnected_) {
    wasConnected_ = true;
    connectionEventPending_ = true;
    registrationPending_ = true;
    lastRegistrationAttempt_ = now - REGISTRATION_RETRY_MS;
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  }

  if (!registrationPending_ || now - lastRegistrationAttempt_ < REGISTRATION_RETRY_MS) {
    return;
  }

  if (time(nullptr) < VALID_TIME_EPOCH) {
    return;
  }

  lastRegistrationAttempt_ = now;
  registrationPending_ = !registerConnection();
}

bool DeviceNetwork::consumeConnectionEvent() {
  const bool eventPending = connectionEventPending_;
  connectionEventPending_ = false;
  return eventPending;
}

bool DeviceNetwork::consumeDisconnectionEvent() {
  const bool eventPending = disconnectionEventPending_;
  disconnectionEventPending_ = false;
  return eventPending;
}

bool DeviceNetwork::registerConnection() {
  if (config_.supabaseUrl == nullptr || config_.deviceId == nullptr ||
      config_.deviceToken == nullptr || config_.supabaseUrl[0] == '\0' ||
      config_.deviceId[0] == '\0' || config_.deviceToken[0] == '\0') {
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.useBuiltinCACertBundle();

  HTTPClient http;
  const String endpoint = String(config_.supabaseUrl) + "/functions/v1/register-device";
  if (!http.begin(secureClient, endpoint)) {
    Serial.println("Impossibile inizializzare HTTPS per la registrazione Supabase");
    return false;
  }

  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + config_.deviceToken);

  const String payload = "{\"device_id\":\"" + jsonEscape(config_.deviceId) +
                         "\",\"ssid\":\"" + jsonEscape(WiFi.SSID()) +
                         "\",\"local_ip\":\"" + WiFi.localIP().toString() + "\"}";
  const int responseCode = http.POST(payload);
  http.end();

  Serial.printf("Registrazione Supabase: HTTP %d\n", responseCode);
  return responseCode >= 200 && responseCode < 300;
}