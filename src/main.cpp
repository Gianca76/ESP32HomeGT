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

  relayRealtime.begin(RELAY_REALTIME_CONFIG, applyRelayTarget);

  if (!deviceNetwork.begin(DEVICE_NETWORK_CONFIG)) {
    Serial.println("Nessuna rete Wi-Fi disponibile; nuovi tentativi in corso");
  }

  server.on("/", HTTP_GET, []() {
    sendPage("/index.html");
  });

  server.on("/info", HTTP_GET, []() {
    sendPage("/info.html");
  });

  server.on("/api/status", HTTP_GET, sendRelayStatus);
  server.on("/api/info", HTTP_GET, sendInfo);

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

  if (networkServicesStarted) {
    ArduinoOTA.handle();
  }
  server.handleClient();
}