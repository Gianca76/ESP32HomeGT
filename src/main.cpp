#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <DeviceNetwork.h>
#include <RelayRealtime.h>
#include "SupabaseRootCAValid.h"
#include "secrets.h"

WebServer server(80);
DeviceNetwork deviceNetwork;
RelayRealtime relayRealtime;

const DeviceNetworkConfig DEVICE_NETWORK_CONFIG = {
  WIFI_CREDENTIALS,
  sizeof(WIFI_CREDENTIALS) / sizeof(WIFI_CREDENTIALS[0]),
  SUPABASE_URL,
  DEVICE_ID,
  DEVICE_REGISTRY_TOKEN,
};

const RelayRealtimeConfig RELAY_REALTIME_CONFIG = {
  SUPABASE_URL,
  SUPABASE_KEY,
  DEVICE_ID,
  DEVICE_AUTH_EMAIL,
  DEVICE_AUTH_PASSWORD,
  SUPABASE_ROOT_CA,
};

constexpr uint8_t RELAY_PINS[] = {32, 33, 25, 26};
constexpr uint8_t RELAY_ON_LEVEL = HIGH;
constexpr uint8_t RELAY_OFF_LEVEL = LOW;

bool relayStates[] = {false, false, false, false};
bool networkServicesStarted = false;
enum class BlindPhase : uint8_t { Idle, HoldUp, HoldDown, WaitForTarget, StopPulse };
constexpr uint32_t BLIND_HOLD_MS = 3200;
constexpr uint32_t BLIND_STOP_PULSE_MS = 250;
constexpr uint32_t BLIND_MIN_TRAVEL_MS = 1000;
constexpr uint32_t BLIND_MAX_TRAVEL_MS = 120000;
BlindPhase blindPhase = BlindPhase::Idle;
uint32_t blindPhaseStartedAt = 0;
uint32_t blindTargetDelayMs = 0;
uint8_t blindTargetPosition = 0;

bool applyRelayTarget(uint8_t relayNumber, bool enabled) {
  if (relayNumber < 1 || relayNumber > 4) {
    return false;
  }

  const uint8_t index = relayNumber - 1;
  if (relayStates[index] == enabled) {
    return false;
  }

  relayStates[index] = enabled;
  digitalWrite(RELAY_PINS[index], enabled ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  return true;
}

void setRelayOutput(uint8_t relayNumber, bool enabled) {
  if (enabled && relayNumber <= 2) {
    const uint8_t oppositeRelay = relayNumber == 1 ? 2 : 1;
    if (applyRelayTarget(oppositeRelay, false)) {
      relayRealtime.reportLocalState(oppositeRelay, false);
    }
  }

  if (applyRelayTarget(relayNumber, enabled)) {
    relayRealtime.reportLocalState(relayNumber, enabled);
  }
}

bool startBlindCommand(uint64_t commandId, uint8_t targetPosition, uint32_t fullTravelMs) {
  const bool needsTravelEstimate = targetPosition > 0 && targetPosition < 100;
  const bool validTravel = fullTravelMs >= BLIND_MIN_TRAVEL_MS &&
                           fullTravelMs <= BLIND_MAX_TRAVEL_MS;
  if (targetPosition > 100 || (needsTravelEstimate && !validTravel) ||
      (!needsTravelEstimate && fullTravelMs != 0 && !validTravel)) {
    return false;
  }

  setRelayOutput(1, false);
  setRelayOutput(2, false);
  relayRealtime.setNetworkWorkDeferred(true);
  blindPhase = targetPosition == 0 ? BlindPhase::HoldDown : BlindPhase::HoldUp;
  blindTargetPosition = targetPosition;
  blindTargetDelayMs = static_cast<uint32_t>(
      (static_cast<uint64_t>(fullTravelMs) * targetPosition) / 100);
  blindPhaseStartedAt = millis();
  setRelayOutput(targetPosition == 0 ? 2 : 1, true);
  return true;
}

void finishBlindSequence() {
  blindPhase = BlindPhase::Idle;
  blindTargetPosition = 0;
  blindTargetDelayMs = 0;
  relayRealtime.setNetworkWorkDeferred(false);
}

void processBlindScenario() {
  const uint32_t now = millis();
  if (blindPhase == BlindPhase::HoldUp && now - blindPhaseStartedAt >= BLIND_HOLD_MS) {
    setRelayOutput(1, false);
    if (blindTargetPosition > 0 && blindTargetPosition < 100) {
      blindPhase = BlindPhase::WaitForTarget;
      blindPhaseStartedAt = now;
    } else {
      finishBlindSequence();
    }
  } else if (blindPhase == BlindPhase::HoldDown && now - blindPhaseStartedAt >= BLIND_HOLD_MS) {
    setRelayOutput(2, false);
    finishBlindSequence();
  } else if (blindPhase == BlindPhase::WaitForTarget &&
             now - blindPhaseStartedAt >= blindTargetDelayMs) {
    blindPhase = BlindPhase::StopPulse;
    blindPhaseStartedAt = now;
    setRelayOutput(2, true);
  } else if (blindPhase == BlindPhase::StopPulse &&
             now - blindPhaseStartedAt >= BLIND_STOP_PULSE_MS) {
    setRelayOutput(2, false);
    finishBlindSequence();
  }
}

void sendPage(const char* path) {
  File page = LittleFS.open(path, "r");
  if (!page) {
    server.send(503, "text/plain; charset=utf-8", "Contenuto web non disponibile");
    return;
  }

  server.streamFile(page, "text/html; charset=utf-8");
  page.close();
}

String jsonEscape(const String& value) {
  String out;
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

void sendRelayStatus() {
  String json = "{\"ip\":\"" + WiFi.localIP().toString() + "\",\"relays\":[";
  for (int i = 0; i < 4; i++) {
    if (i) json += ",";
    json += relayStates[i] ? "true" : "false";
  }
  json += "]";
  json += ",\"realtime\":";
  json += relayRealtime.isSubscribed() ? "true" : "false";
  json += ",\"broadcasts\":" + String(relayRealtime.receivedBroadcastCount());
  json += ",\"parsedBroadcasts\":" + String(relayRealtime.parsedBroadcastCount());
  json += ",\"wsTextFrames\":" + String(relayRealtime.receivedTextFrameCount());
  json += ",\"wsBinaryFrames\":" + String(relayRealtime.receivedBinaryFrameCount()) + "}";
  server.send(200, "application/json", json);
}

void handleRelay(int index) {
  if (index < 2) {
    server.send(409, "application/json", "{\"error\":\"usa i comandi tapparella\"}");
    return;
  }

  if (!server.hasArg("state")) {
    server.send(400, "application/json", "{\"error\":\"state mancante\"}");
    return;
  }

  const bool enabled = server.arg("state") == "1";
  relayStates[index] = enabled;
  digitalWrite(RELAY_PINS[index], enabled ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  relayRealtime.reportLocalState(static_cast<uint8_t>(index + 1), enabled);
  sendRelayStatus();
}

void handleBlindCommand() {
  if (!server.hasArg("target_position") || !server.hasArg("full_travel_ms")) {
    server.send(400, "application/json", "{\"error\":\"posizione o tempo di corsa mancanti\"}");
    return;
  }

  const uint32_t targetPosition = strtoul(server.arg("target_position").c_str(), nullptr, 10);
  const uint32_t fullTravelMs = strtoul(server.arg("full_travel_ms").c_str(), nullptr, 10);
  if (targetPosition > 100 || !startBlindCommand(0, static_cast<uint8_t>(targetPosition), fullTravelMs)) {
    server.send(400, "application/json", "{\"error\":\"posizione o tempo di corsa non valido\"}");
    return;
  }

  server.send(202, "application/json", "{\"accepted\":true}");
}

void sendInfo() {
  const uint64_t seconds = millis() / 1000;
  const String uptime = String(seconds / 86400) + "g " +
                        String((seconds / 3600) % 24) + "h " +
                        String((seconds / 60) % 60) + "m";

  String json = "{";
  json += "\"ip\":\"" + jsonEscape(WiFi.localIP().toString()) + "\",";
  json += "\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime\":\"" + uptime + "\",";
  json += "\"chip\":\"" + jsonEscape(String(ESP.getChipModel())) + "\",";
  json += "\"revision\":" + String(ESP.getChipRevision()) + ",";
  json += "\"flash\":\"" + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB\",";
  json += "\"heap\":\"" + String(ESP.getFreeHeap() / 1024) + " KB\"}";
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
    Serial.println("Errore montaggio LittleFS");
  }

  // Imposta subito lo stato iniziale dei quattro canali.
  for (int i = 0; i < 4; i++) {
    digitalWrite(RELAY_PINS[i], RELAY_OFF_LEVEL);
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  relayRealtime.begin(RELAY_REALTIME_CONFIG, applyRelayTarget, startBlindCommand);

  if (!deviceNetwork.begin(DEVICE_NETWORK_CONFIG)) {
    Serial.println("Nessuna rete Wi-Fi disponibile; nuovi tentativi in corso");
  }

  server.on("/", HTTP_GET, []() {
    sendPage("/index.html");
  });

  server.on("/blinds", HTTP_GET, []() {
    sendPage("/blinds.html");
  });

  server.on("/info", HTTP_GET, []() {
    sendPage("/info.html");
  });

  server.on("/api/status", HTTP_GET, sendRelayStatus);
  server.on("/api/info", HTTP_GET, sendInfo);
  server.on("/api/blinds", HTTP_POST, handleBlindCommand);

  server.on("/api/relay/1", HTTP_POST, []() { handleRelay(0); });
  server.on("/api/relay/2", HTTP_POST, []() { handleRelay(1); });
  server.on("/api/relay/3", HTTP_POST, []() { handleRelay(2); });
  server.on("/api/relay/4", HTTP_POST, []() { handleRelay(3); });

  server.onNotFound([]() {
    server.send(404, "text/plain; charset=utf-8", "Pagina non trovata");
  });

  server.begin();
  Serial.println("Webserver pronto");
}

void loop() {
  processBlindScenario();
  deviceNetwork.loop();

  if (deviceNetwork.consumeDisconnectionEvent()) {
    if (networkServicesStarted) {
      ArduinoOTA.end();
      networkServicesStarted = false;
    }
    relayRealtime.onNetworkDisconnected();
  }

  if (deviceNetwork.consumeConnectionEvent()) {
    relayRealtime.onNetworkConnected();
    Serial.print("Connesso a ");
    Serial.print(WiFi.SSID());
    Serial.print(" con IP ");
    Serial.println(WiFi.localIP());

    if (!networkServicesStarted) {
      ArduinoOTA.setHostname("esp32-relay");
      ArduinoOTA.begin();
      networkServicesStarted = true;
    }
  }

  relayRealtime.loop();
  processBlindScenario();

  if (networkServicesStarted) {
    ArduinoOTA.handle();
  }
  server.handleClient();
  processBlindScenario();
}