#pragma once

#include <Arduino.h>
#include <WiFiMulti.h>

struct WifiCredential {
  const char* ssid;
  const char* password;
};

struct DeviceNetworkConfig {
  const WifiCredential* wifiCredentials;
  size_t wifiCredentialCount;
  const char* supabaseUrl;
  const char* deviceId;
  const char* deviceToken;
};

class DeviceNetwork {
public:
  bool begin(const DeviceNetworkConfig& config);
  void loop();
  bool consumeConnectionEvent();
  bool consumeDisconnectionEvent();

private:
  bool registerConnection();

  WiFiMulti wifiMulti_;
  DeviceNetworkConfig config_{};
  bool wasConnected_ = false;
  bool connectionEventPending_ = false;
  bool disconnectionEventPending_ = false;
  bool registrationPending_ = false;
  uint32_t lastReconnectAttempt_ = 0;
  uint32_t lastRegistrationAttempt_ = 0;
};