#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebSocketsClient.h>

struct RelayRealtimeConfig {
  const char* supabaseUrl;
  const char* publishableKey;
  const char* deviceId;
  const char* authEmail;
  const char* authPassword;
  const char* rootCa;
};

using RelayTargetHandler = bool (*)(uint8_t relayNumber, bool enabled);

class RelayRealtime {
public:
  bool begin(const RelayRealtimeConfig& config, RelayTargetHandler targetHandler);
  void onNetworkConnected();
  void onNetworkDisconnected();
  void loop();
  void reportLocalState(uint8_t relayNumber, bool enabled);

private:
  void handleWebSocketEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleMessage(const uint8_t* payload, size_t length);
  void sendJoin();
  void sendHeartbeat();
  void sendAccessToken();
  bool authenticate();
  bool requestToken(bool refresh);
  bool loadRelaySnapshot();
  bool reportRelayState(uint8_t relayNumber, bool enabled, const char* source);
  void queueReport(uint8_t relayNumber, bool enabled, const char* source);
  void processPendingReports();
  String supabaseHost() const;

  WebSocketsClient webSocket_;
  Preferences preferences_;
  RelayRealtimeConfig config_{};
  RelayTargetHandler targetHandler_ = nullptr;
  String accessToken_;
  String refreshToken_;
  String topic_;
  bool started_ = false;
  bool joined_ = false;
  bool snapshotPending_ = false;
  bool authPending_ = false;
  uint32_t nextAuthAttempt_ = 0;
  uint32_t tokenRefreshAt_ = 0;
  uint32_t nextHeartbeatAt_ = 0;
  uint32_t nextReportAttempt_ = 0;
  uint32_t messageRef_ = 1;
  bool pendingReports_[4] = {false, false, false, false};
  bool pendingStates_[4] = {false, false, false, false};
  String pendingSources_[4];
};