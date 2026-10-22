#include "RelayRealtime.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

namespace {
constexpr uint32_t AUTH_RETRY_MS = 15000;
constexpr uint32_t REPORT_RETRY_MS = 10000;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 20000;
constexpr time_t VALID_TIME_EPOCH = 1700000000;
}

bool RelayRealtime::begin(const RelayRealtimeConfig& config, RelayTargetHandler targetHandler,
                          BlindCommandHandler blindCommandHandler) {
  config_ = config;
  targetHandler_ = targetHandler;
  blindCommandHandler_ = blindCommandHandler;
  topic_ = String("device:") + config_.deviceId;
  preferences_.begin("sb-relay", false);
  refreshToken_ = preferences_.getString("refresh", "");
  lastBlindCommandId_ = preferences_.getULong64("blindcmd", 0);
  webSocket_.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    handleWebSocketEvent(type, payload, length);
  });
  webSocket_.setReconnectInterval(5000);
  webSocket_.enableHeartbeat(45000, 10000, 2);
  authPending_ = true;
  nextAuthAttempt_ = millis();
  return config_.supabaseUrl != nullptr && config_.publishableKey != nullptr &&
         config_.deviceId != nullptr && config_.authEmail != nullptr &&
         config_.authPassword != nullptr && config_.rootCa != nullptr;
}

void RelayRealtime::onNetworkConnected() {
  authPending_ = true;
  nextAuthAttempt_ = millis();
}

void RelayRealtime::onNetworkDisconnected() {
  if (started_) {
    webSocket_.disconnect();
  }
  started_ = false;
  joined_ = false;
  snapshotPending_ = false;
  accessToken_ = "";
  authPending_ = true;
}

void RelayRealtime::setNetworkWorkDeferred(bool deferred) {
  networkWorkDeferred_ = deferred;
}

void RelayRealtime::loop() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (started_) {
    webSocket_.loop();
  }

  const uint32_t now = millis();
  if (!networkWorkDeferred_ && authPending_ && static_cast<int32_t>(now - nextAuthAttempt_) >= 0 &&
      time(nullptr) >= VALID_TIME_EPOCH) {
    nextAuthAttempt_ = now + AUTH_RETRY_MS;
    if (authenticate()) {
      authPending_ = false;
      if (!started_) {
        const String path = String("/realtime/v1/websocket?apikey=") +
                            config_.publishableKey + "&vsn=2.0.0";
        webSocket_.beginSslWithCA(supabaseHost().c_str(), 443, path.c_str(), config_.rootCa);
        started_ = true;
      }
    }
  }

  if (!networkWorkDeferred_ && !accessToken_.isEmpty() &&
      static_cast<int32_t>(now - tokenRefreshAt_) >= 0) {
    if (requestToken(true)) {
      if (joined_) sendAccessToken();
    } else {
      authPending_ = true;
      nextAuthAttempt_ = now + AUTH_RETRY_MS;
      joined_ = false;
      webSocket_.disconnect();
      started_ = false;
      accessToken_ = "";
    }
  }

  if (joined_ && static_cast<int32_t>(now - nextHeartbeatAt_) >= 0) {
    sendHeartbeat();
    nextHeartbeatAt_ = now + HEARTBEAT_INTERVAL_MS;
  }

  if (!networkWorkDeferred_) processPendingReports();
  if (!networkWorkDeferred_ && joined_ && snapshotPending_ && !pendingReports_[0] && !pendingReports_[1] &&
      !pendingReports_[2] && !pendingReports_[3]) {
    snapshotPending_ = false;
    loadRelaySnapshot();
  }
}

void RelayRealtime::reportLocalState(uint8_t relayNumber, bool enabled) {
  queueReport(relayNumber, enabled, "local");
}

bool RelayRealtime::isSubscribed() const {
  return joined_;
}

uint32_t RelayRealtime::receivedBroadcastCount() const {
  return receivedBroadcastCount_;
}

uint32_t RelayRealtime::receivedTextFrameCount() const {
  return receivedTextFrameCount_;
}

uint32_t RelayRealtime::receivedBinaryFrameCount() const {
  return receivedBinaryFrameCount_;
}

uint32_t RelayRealtime::parsedBroadcastCount() const {
  return parsedBroadcastCount_;
}

void RelayRealtime::handleWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.println("Supabase Realtime: WebSocket connesso");
    joined_ = false;
    sendJoin();
  } else if (type == WStype_DISCONNECTED) {
    Serial.println("Supabase Realtime: WebSocket disconnesso");
    joined_ = false;
  } else if (type == WStype_TEXT) {
    receivedTextFrameCount_++;
    handleMessage(payload, length);
  } else if (type == WStype_BIN) {
    receivedBinaryFrameCount_++;
    handleBinaryMessage(payload, length);
  } else if (type == WStype_ERROR) {
    Serial.println("Supabase Realtime: errore WebSocket");
  }
}

void RelayRealtime::handleMessage(const uint8_t* payload, size_t length) {
  JsonDocument message;
  if (deserializeJson(message, payload, length)) {
    return;
  }

  JsonArrayConst frame = message.as<JsonArrayConst>();
  if (frame.size() < 5) {
    return;
  }

  const char* event = frame[3] | "";
  JsonObjectConst eventPayload = frame[4].as<JsonObjectConst>();
  const String frameTopic = frame[2] | "";
  const String frameRef = frame[1] | "";
  if (strcmp(event, "phx_reply") == 0 && frameTopic == String("realtime:") + topic_ &&
      frameRef == "1" && eventPayload["status"] == "ok") {
    joined_ = true;
    snapshotPending_ = true;
    nextHeartbeatAt_ = millis() + HEARTBEAT_INTERVAL_MS;
    Serial.println("Supabase Realtime: subscription attiva");
    return;
  }

  if (frameTopic == String("realtime:") + topic_ &&
      (strcmp(event, "phx_close") == 0 || strcmp(event, "phx_error") == 0 ||
       (strcmp(event, "system") == 0 && eventPayload["status"] == "error"))) {
    joined_ = false;
    webSocket_.disconnect();
    return;
  }

  if (strcmp(event, "broadcast") != 0) {
    return;
  }

  const String topic = frameTopic.startsWith("realtime:")
      ? frameTopic.substring(String("realtime:").length())
      : frameTopic;
  const String eventName = eventPayload["event"] | "";
  handleBroadcast(topic, eventName, eventPayload["payload"].as<JsonVariantConst>());
}

void RelayRealtime::handleBinaryMessage(const uint8_t* payload, size_t length) {
  if (length < 5 || payload[0] != 0x04 || payload[4] != 0x01) {
    return;
  }

  const size_t topicLength = payload[1];
  const size_t eventLength = payload[2];
  const size_t metadataLength = payload[3];
  const size_t headerLength = 5 + topicLength + eventLength + metadataLength;
  if (headerLength > length) {
    return;
  }

  size_t offset = 5;
  String topic(reinterpret_cast<const char*>(payload + offset), topicLength);
  if (topic.startsWith("realtime:")) {
    topic.remove(0, String("realtime:").length());
  }
  offset += topicLength;
  const String event(reinterpret_cast<const char*>(payload + offset), eventLength);
  offset += eventLength + metadataLength;

  JsonDocument eventPayload;
  if (deserializeJson(eventPayload, payload + offset, length - offset)) {
    return;
  }

  handleBroadcast(topic, event, eventPayload.as<JsonVariantConst>());
}

void RelayRealtime::handleBroadcast(
    const String& topic, const String& event, JsonVariantConst payload) {
  parsedBroadcastCount_++;
  if (topic != topic_ || (event != "relay_state" && event != "device_presence" &&
                          event != "blind_command")) {
    return;
  }

  JsonObjectConst state = payload.as<JsonObjectConst>();
  if (state["device_id"] != config_.deviceId) {
    return;
  }
  receivedBroadcastCount_++;

  if (event == "device_presence") {
    Serial.println("Supabase Realtime: presenza dispositivo aggiornata");
    return;
  }

  if (event == "blind_command") {
    const uint64_t commandId = state["command_id"] | 0;
    const uint32_t targetPosition = state["target_position"] | 255;
    const uint32_t fullTravelMs = state["full_travel_ms"] | 0;
    if (targetPosition <= 100) {
      processBlindCommand(commandId, static_cast<uint8_t>(targetPosition), fullTravelMs);
    }
    return;
  }

  const uint8_t relayNumber = state["relay_number"] | 0;
  const bool desiredState = state["desired_state"] | false;
  if (relayNumber >= 3 && relayNumber <= 4 && targetHandler_ != nullptr) {
    targetHandler_(relayNumber, desiredState);
    queueReport(relayNumber, desiredState, "remote");
  }
}

void RelayRealtime::sendJoin() {
  JsonDocument message;
  JsonArray frame = message.to<JsonArray>();
  frame.add("1");
  frame.add("1");
  frame.add(String("realtime:") + topic_);
  frame.add("phx_join");

  JsonObject payload = frame.add<JsonObject>();
  JsonObject config = payload["config"].to<JsonObject>();
  config["broadcast"]["ack"] = false;
  config["broadcast"]["self"] = false;
  config["presence"]["enabled"] = false;
  config["postgres_changes"].to<JsonArray>();
  config["private"] = true;
  payload["access_token"] = accessToken_;

  String serialized;
  serializeJson(message, serialized);
  webSocket_.sendTXT(serialized);
}

void RelayRealtime::sendHeartbeat() {
  JsonDocument message;
  JsonArray frame = message.to<JsonArray>();
  frame.add(nullptr);
  frame.add(String(messageRef_++));
  frame.add("phoenix");
  frame.add("heartbeat");
  frame.add<JsonObject>();
  String serialized;
  serializeJson(message, serialized);
  webSocket_.sendTXT(serialized);
}

void RelayRealtime::sendAccessToken() {
  JsonDocument message;
  JsonArray frame = message.to<JsonArray>();
  frame.add("1");
  frame.add(String(messageRef_++));
  frame.add(String("realtime:") + topic_);
  frame.add("access_token");
  frame.add<JsonObject>()["access_token"] = accessToken_;
  String serialized;
  serializeJson(message, serialized);
  webSocket_.sendTXT(serialized);
}

bool RelayRealtime::authenticate() {
  if (!refreshToken_.isEmpty() && requestToken(true)) {
    return true;
  }
  return requestToken(false);
}

bool RelayRealtime::requestToken(bool refresh) {
  WiFiClientSecure secureClient;
  secureClient.useBuiltinCACertBundle();

  HTTPClient http;
  const String endpoint = String(config_.supabaseUrl) + "/auth/v1/token?grant_type=" +
                          (refresh ? "refresh_token" : "password");
  if (!http.begin(secureClient, endpoint)) {
    return false;
  }

  JsonDocument request;
  if (refresh) {
    request["refresh_token"] = refreshToken_;
  } else {
    request["email"] = config_.authEmail;
    request["password"] = config_.authPassword;
  }

  String body;
  serializeJson(request, body);
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.addHeader("apikey", config_.publishableKey);
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(body);
  const String response = http.getString();
  http.end();

  if (status < 200 || status >= 300) {
    Serial.printf("Supabase Auth: HTTP %d\n", status);
    return false;
  }

  JsonDocument tokenResponse;
  if (deserializeJson(tokenResponse, response)) {
    return false;
  }

  accessToken_ = tokenResponse["access_token"].as<String>();
  refreshToken_ = tokenResponse["refresh_token"].as<String>();
  if (accessToken_.isEmpty() || refreshToken_.isEmpty()) {
    return false;
  }

  preferences_.putString("refresh", refreshToken_);
  const uint32_t expiresIn = tokenResponse["expires_in"] | 3600;
  const uint32_t refreshIn = expiresIn > 120 ? expiresIn - 120 : 60;
  tokenRefreshAt_ = millis() + refreshIn * 1000UL;
  return true;
}

bool RelayRealtime::loadRelaySnapshot() {
  WiFiClientSecure secureClient;
  secureClient.useBuiltinCACertBundle();

  HTTPClient http;
  const String endpoint = String(config_.supabaseUrl) +
      "/rest/v1/relay_state?select=device_id,relay_number,desired_state,actual_state&device_id=eq." +
      config_.deviceId + "&order=relay_number.asc";
  if (!http.begin(secureClient, endpoint)) {
    return false;
  }

  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.addHeader("apikey", config_.publishableKey);
  http.addHeader("Authorization", String("Bearer ") + accessToken_);
  const int status = http.GET();
  const String response = http.getString();
  http.end();

  if (status < 200 || status >= 300) {
    Serial.printf("Lettura stato relè: HTTP %d\n", status);
    return false;
  }

  JsonDocument snapshot;
  if (deserializeJson(snapshot, response)) {
    return false;
  }

  for (JsonObjectConst row : snapshot.as<JsonArrayConst>()) {
    const uint8_t relayNumber = row["relay_number"] | 0;
    const bool desiredState = row["desired_state"] | false;
    if (relayNumber >= 1 && relayNumber <= 2) {
      queueReport(relayNumber, false, "local");
    } else if (relayNumber >= 3 && relayNumber <= 4 && targetHandler_ != nullptr) {
      targetHandler_(relayNumber, desiredState);
      queueReport(relayNumber, desiredState, "reconcile");
    }
  }
  loadRecentBlindCommand();
  return true;
}

bool RelayRealtime::loadRecentBlindCommand() {
  WiFiClientSecure secureClient;
  secureClient.useBuiltinCACertBundle();

  HTTPClient http;
  const String endpoint = String(config_.supabaseUrl) +
      "/rest/v1/rpc/get_recent_blind_command";
  if (!http.begin(secureClient, endpoint)) {
    return false;
  }

  JsonDocument request;
  request["p_device_id"] = config_.deviceId;
  String body;
  serializeJson(request, body);

  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.addHeader("apikey", config_.publishableKey);
  http.addHeader("Authorization", String("Bearer ") + accessToken_);
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(body);
  const String response = http.getString();
  http.end();

  if (status < 200 || status >= 300) {
    Serial.printf("Lettura ultimo comando tapparella: HTTP %d\n", status);
    return false;
  }

  JsonDocument commandSnapshot;
  if (deserializeJson(commandSnapshot, response)) {
    return false;
  }
  const JsonArrayConst commands = commandSnapshot.as<JsonArrayConst>();
  if (commands.isNull() || commands.size() == 0) {
    return true;
  }

  const JsonObjectConst command = commands[0];
  const uint64_t commandId = command["command_id"] | 0;
  const uint32_t targetPosition = command["target_position"] | 255;
  const uint32_t fullTravelMs = command["full_travel_ms"] | 0;
  return targetPosition <= 100 && processBlindCommand(
      commandId, static_cast<uint8_t>(targetPosition), fullTravelMs);
}

bool RelayRealtime::processBlindCommand(
    uint64_t commandId, uint8_t targetPosition, uint32_t fullTravelMs) {
  if (commandId == 0 || commandId <= lastBlindCommandId_ ||
      targetHandler_ == nullptr || blindCommandHandler_ == nullptr) {
    return false;
  }

  if (!blindCommandHandler_(commandId, targetPosition, fullTravelMs)) {
    return false;
  }

  lastBlindCommandId_ = commandId;
  preferences_.putULong64("blindcmd", commandId);
  return true;
}

bool RelayRealtime::reportRelayState(uint8_t relayNumber, bool enabled, const char* source) {
  WiFiClientSecure secureClient;
  secureClient.useBuiltinCACertBundle();

  HTTPClient http;
  const String endpoint = String(config_.supabaseUrl) + "/rest/v1/rpc/report_relay_state";
  if (!http.begin(secureClient, endpoint)) {
    return false;
  }

  JsonDocument request;
  request["p_device_id"] = config_.deviceId;
  request["p_relay_number"] = relayNumber;
  request["p_enabled"] = enabled;
  request["p_source"] = source;
  String body;
  serializeJson(request, body);

  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.addHeader("apikey", config_.publishableKey);
  http.addHeader("Authorization", String("Bearer ") + accessToken_);
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(body);
  http.end();
  return status >= 200 && status < 300;
}

void RelayRealtime::queueReport(uint8_t relayNumber, bool enabled, const char* source) {
  if (relayNumber < 1 || relayNumber > 4) {
    return;
  }
  const uint8_t index = relayNumber - 1;
  pendingReports_[index] = true;
  pendingStates_[index] = enabled;
  pendingSources_[index] = source;
}

void RelayRealtime::processPendingReports() {
  if (accessToken_.isEmpty() || static_cast<int32_t>(millis() - nextReportAttempt_) < 0) {
    return;
  }

  for (uint8_t index = 0; index < 4; index++) {
    if (!pendingReports_[index]) continue;
    nextReportAttempt_ = millis() + REPORT_RETRY_MS;
    if (reportRelayState(index + 1, pendingStates_[index], pendingSources_[index].c_str())) {
      pendingReports_[index] = false;
    }
    return;
  }
}

String RelayRealtime::supabaseHost() const {
  String host = config_.supabaseUrl;
  host.replace("https://", "");
  host.replace("http://", "");
  const int slash = host.indexOf('/');
  if (slash >= 0) host.remove(slash);
  return host;
}