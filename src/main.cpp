#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "secrets.h"

WebServer server(80);

constexpr uint8_t RELAY_PINS[] = {32, 33, 25, 26};
constexpr uint8_t RELAY_ON_LEVEL = HIGH;
constexpr uint8_t RELAY_OFF_LEVEL = LOW;

bool relayStates[] = {false, false, false, false};

const char CONTROL_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#101522">
  <title>Relay Studio</title>
  <style>
    :root {
      color-scheme: dark;
      --bg:#101522; --panel:#1a2232; --panel2:#202b3d;
      --text:#f4f6fb; --muted:#929db0; --cyan:#75e0d1;
      --gold:#ffca7a; --line:#ffffff12; --green:#64e0ab;
    }
    * { box-sizing:border-box }
    body {
      margin:0; min-height:100vh; color:var(--text);
      background:
        radial-gradient(ellipse at 10% -10%, #294b59 0, transparent 38%),
        radial-gradient(ellipse at 100% 5%, #463247 0, transparent 30%),
        var(--bg);
      font:16px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;
    }
    .wrap { width:min(100% - 28px, 620px); margin:auto; padding:22px 0 40px }
    header { display:flex; align-items:center; justify-content:space-between; gap:12px }
    .brand { display:flex; align-items:center; gap:10px; font-weight:750; letter-spacing:.02em }
    .mark {
      display:grid; place-items:center; width:38px; height:38px; border-radius:13px;
      color:#10201f; background:linear-gradient(145deg,#a3f4df,#62c9d6);
      box-shadow:0 8px 25px #68ddcf30; font-size:20px
    }
    nav {
      display:flex; gap:4px; padding:4px; border:1px solid var(--line);
      border-radius:14px; background:#ffffff08
    }
    nav a {
      padding:8px 12px; color:var(--muted); text-decoration:none;
      border-radius:10px; font-size:13px; font-weight:650
    }
    nav a.active { color:var(--text); background:#ffffff12 }
    .hero { padding:32px 2px 22px }
    .eyebrow { color:var(--cyan); font-size:11px; font-weight:800; letter-spacing:.16em; text-transform:uppercase }
    h1 { margin:8px 0 7px; font-size:clamp(29px,8vw,42px); line-height:1.05; letter-spacing:-.04em }
    .sub { margin:0; color:var(--muted); font-size:14px }
    .connection {
      display:flex; align-items:center; gap:9px; margin:20px 0 17px;
      padding:12px 14px; border:1px solid var(--line); border-radius:15px;
      background:#ffffff08; color:#d9e0ea; font-size:13px
    }
    .dot { width:9px; height:9px; border-radius:50%; background:var(--green); box-shadow:0 0 13px #64e0ab88 }
    .ip { margin-left:auto; color:var(--muted); font:12px ui-monospace,monospace }
    .section-title { margin:24px 2px 12px; display:flex; justify-content:space-between; align-items:center }
    .section-title h2 { margin:0; font-size:16px }
    .section-title span { color:var(--muted); font-size:12px }
    .grid { display:grid; grid-template-columns:1fr 1fr; gap:11px }
    .relay {
      min-height:158px; padding:15px; border:1px solid var(--line); border-radius:20px;
      text-align:left; color:var(--text); background:linear-gradient(145deg,var(--panel2),var(--panel));
      box-shadow:0 12px 28px #0002; cursor:pointer; transition:.18s transform,.18s border-color,.18s background;
      -webkit-tap-highlight-color:transparent
    }
    .relay:active { transform:scale(.975) }
    .relay.on { border-color:#75e0d188; background:linear-gradient(145deg,#23453f,#1b3037) }
    .top { display:flex; align-items:center; justify-content:space-between }
    .number {
      display:grid; place-items:center; width:34px; height:34px; border-radius:11px;
      background:#ffffff0c; color:var(--gold); font-size:12px; font-weight:800
    }
    .switch {
      width:42px; height:25px; padding:3px; border-radius:20px; background:#3a4353;
      transition:background .18s
    }
    .switch::after {
      content:""; display:block; width:19px; height:19px; border-radius:50%;
      background:#c6ccd6; transition:transform .18s,background .18s
    }
    .relay.on .switch { background:#54cdb2 }
    .relay.on .switch::after { transform:translateX(17px); background:#fff }
    .name { margin-top:16px; font-size:16px; font-weight:730 }
    .hint { margin-top:3px; color:var(--muted); font-size:12px }
    .state { display:inline-block; margin-top:12px; color:var(--muted); font-size:10px; font-weight:850; letter-spacing:.13em }
    .relay.on .state { color:var(--cyan) }
    button:disabled { opacity:.65; cursor:wait }
    footer { margin:21px 3px 0; color:var(--muted); font-size:11px; text-align:center }
    @media(max-width:370px) { .wrap { width:calc(100% - 20px) } .grid { gap:8px } .relay { padding:12px; min-height:150px } nav a { padding:8px 9px } }
  </style>
</head>
<body>
  <div class="wrap">
    <header>
      <div class="brand"><span class="mark">ϟ</span><span>Relay Studio</span></div>
      <nav><a class="active" href="/">Controllo</a><a href="/info">Sistema</a></nav>
    </header>

    <section class="hero">
      <div class="eyebrow">ESP32 · Home control</div>
      <h1>La tua casa,<br>un tocco alla volta.</h1>
      <p class="sub">Controllo locale dei quattro canali relè.</p>
    </section>

    <div class="connection">
      <span class="dot"></span><span id="connection">ESP32 connesso</span>
      <span class="ip" id="ip">Wi‑Fi</span>
    </div>

    <div class="section-title">
      <h2>Uscite</h2><span>tocca una scheda per cambiare stato</span>
    </div>

    <main class="grid">
      <button class="relay" id="r0" onclick="toggleRelay(0)" aria-pressed="false">
        <span class="top"><span class="number">01</span><span class="switch"></span></span>
        <span class="name">Relè uno</span><span class="hint">Canale 1</span>
        <span class="state">SPENTO</span>
      </button>
      <button class="relay" id="r1" onclick="toggleRelay(1)" aria-pressed="false">
        <span class="top"><span class="number">02</span><span class="switch"></span></span>
        <span class="name">Relè due</span><span class="hint">Canale 2</span>
        <span class="state">SPENTO</span>
      </button>
      <button class="relay" id="r2" onclick="toggleRelay(2)" aria-pressed="false">
        <span class="top"><span class="number">03</span><span class="switch"></span></span>
        <span class="name">Relè tre</span><span class="hint">Canale 3</span>
        <span class="state">SPENTO</span>
      </button>
      <button class="relay" id="r3" onclick="toggleRelay(3)" aria-pressed="false">
        <span class="top"><span class="number">04</span><span class="switch"></span></span>
        <span class="name">Relè quattro</span><span class="hint">Canale 4</span>
        <span class="state">SPENTO</span>
      </button>
    </main>

    <footer>Comandi inviati direttamente alla scheda sulla rete locale</footer>
  </div>

  <script>
    let states = [false, false, false, false];

    function paint(i, on) {
      states[i] = on;
      const b = document.getElementById("r" + i);
      b.classList.toggle("on", on);
      b.setAttribute("aria-pressed", String(on));
      b.querySelector(".state").textContent = on ? "ACCESO" : "SPENTO";
    }

    async function refresh() {
      try {
        const r = await fetch("/api/status", {cache:"no-store"});
        const d = await r.json();
        d.relays.forEach((v, i) => paint(i, v));
        document.getElementById("ip").textContent = d.ip;
        document.getElementById("connection").textContent = "ESP32 connesso";
      } catch (_) {
        document.getElementById("connection").textContent = "Connessione interrotta";
      }
    }

    async function toggleRelay(i) {
      const b = document.getElementById("r" + i);
      b.disabled = true;
      try {
        const r = await fetch("/api/relay/" + (i + 1), {
          method:"POST",
          headers:{"Content-Type":"application/x-www-form-urlencoded"},
          body:"state=" + (states[i] ? "0" : "1")
        });
        const d = await r.json();
        d.relays.forEach((v, n) => paint(n, v));
      } catch (_) {
        document.getElementById("connection").textContent = "Invio comando non riuscito";
      }
      b.disabled = false;
    }

    refresh();
    setInterval(refresh, 3000);
  </script>
</body>
</html>
)HTML";

const char INFO_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#101522">
  <title>Sistema · Relay Studio</title>
  <style>
    :root { color-scheme:dark; --bg:#101522; --panel:#1b2535; --muted:#929db0; --cyan:#75e0d1; --line:#ffffff12 }
    * { box-sizing:border-box }
    body { margin:0; min-height:100vh; color:#f4f6fb; background:radial-gradient(ellipse at 10% -10%,#294b59,transparent 38%),var(--bg); font:16px/1.45 system-ui,sans-serif }
    .wrap { width:min(100% - 28px,620px); margin:auto; padding:22px 0 40px }
    header { display:flex; align-items:center; justify-content:space-between; gap:10px }
    .brand { font-weight:750 } nav { display:flex; gap:4px; padding:4px; border:1px solid var(--line); border-radius:14px }
    nav a { padding:8px 12px; color:var(--muted); text-decoration:none; border-radius:10px; font-size:13px; font-weight:650 }
    nav a.active { color:#fff; background:#ffffff12 }
    h1 { margin:36px 0 5px; font-size:34px; letter-spacing:-.04em }
    .sub { margin:0 0 22px; color:var(--muted); font-size:14px }
    .grid { display:grid; grid-template-columns:1fr 1fr; gap:10px }
    .item { padding:15px; border:1px solid var(--line); border-radius:17px; background:var(--panel); min-width:0 }
    .label { color:var(--muted); font-size:11px; text-transform:uppercase; letter-spacing:.12em }
    .value { margin-top:7px; overflow-wrap:anywhere; font-size:15px; font-weight:700 }
    .wide { grid-column:1/-1 }
    .back { display:block; margin-top:20px; color:var(--cyan); text-decoration:none }
    @media(max-width:360px) { .grid { grid-template-columns:1fr } .wide { grid-column:auto } }
  </style>
</head>
<body>
  <div class="wrap">
    <header><div class="brand">ϟ Relay Studio</div><nav><a href="/">Controllo</a><a class="active" href="/info">Sistema</a></nav></header>
    <h1>Il sistema</h1>
    <p class="sub">Stato e informazioni utili sulla scheda.</p>
    <main class="grid">
      <div class="item"><div class="label">Indirizzo IP</div><div class="value" id="ip">…</div></div>
      <div class="item"><div class="label">Wi‑Fi</div><div class="value" id="ssid">…</div></div>
      <div class="item"><div class="label">Segnale</div><div class="value" id="rssi">…</div></div>
      <div class="item"><div class="label">Uptime</div><div class="value" id="uptime">…</div></div>
      <div class="item"><div class="label">Chip</div><div class="value" id="chip">…</div></div>
      <div class="item"><div class="label">Flash</div><div class="value" id="flash">…</div></div>
      <div class="item wide"><div class="label">Memoria libera</div><div class="value" id="heap">…</div></div>
    </main>
    <a class="back" href="/">← Torna ai controlli</a>
  </div>
  <script>
    async function updateInfo() {
      const d = await (await fetch("/api/info", {cache:"no-store"})).json();
      document.getElementById("ip").textContent = d.ip;
      document.getElementById("ssid").textContent = d.ssid;
      document.getElementById("rssi").textContent = d.rssi + " dBm";
      document.getElementById("uptime").textContent = d.uptime;
      document.getElementById("chip").textContent = d.chip + " · rev. " + d.revision;
      document.getElementById("flash").textContent = d.flash;
      document.getElementById("heap").textContent = d.heap;
    }
    updateInfo();
    setInterval(updateInfo, 5000);
  </script>
</body>
</html>
)HTML";

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
  json += "]}";
  server.send(200, "application/json", json);
}

void handleRelay(int index) {
  if (!server.hasArg("state")) {
    server.send(400, "application/json", "{\"error\":\"state mancante\"}");
    return;
  }

  relayStates[index] = server.arg("state") == "1";
  digitalWrite(RELAY_PINS[index],
               relayStates[index] ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
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

  // Imposta subito lo stato iniziale dei quattro canali.
  for (int i = 0; i < 4; i++) {
    digitalWrite(RELAY_PINS[i], RELAY_OFF_LEVEL);
    pinMode(RELAY_PINS[i], OUTPUT);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connessione Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  ArduinoOTA.setHostname("esp32-relay");
  ArduinoOTA.begin();

  MDNS.begin("esp32-relay");

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", CONTROL_PAGE);
  });

  server.on("/info", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", INFO_PAGE);
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
  Serial.println("Webserver e OTA pronti");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
}