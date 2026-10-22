import { createClient } from "https://cdn.jsdelivr.net/npm/@supabase/supabase-js@2.91.0/+esm";
import { SUPABASE_PUBLISHABLE_KEY, SUPABASE_URL } from "./config.js";

const supabase = createClient(SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY);
const loginPanel = document.querySelector("#login-panel");
const dashboard = document.querySelector("#dashboard");
const devicesSelect = document.querySelector("#device-select");
const travelInput = document.querySelector("#travel-time");
const notice = document.querySelector("#notice");
const positionButtons = [...document.querySelectorAll(".position")];
let activeDeviceId = null;
let activeUserId = null;
let activeLoadId = 0;
let deviceChannel = null;

const storedTravel = localStorage.getItem("blind-full-travel-seconds");
if (storedTravel) travelInput.value = storedTravel;

function setConnection(state, label) {
  const connection = document.querySelector("#connection");
  connection.dataset.state = state;
  document.querySelector("#connection-label").textContent = label;
}

function validTravel() {
  const seconds = Number(travelInput.value);
  return Number.isFinite(seconds) && seconds >= 1 && seconds <= 120;
}

function setPreview(position) {
  document.querySelector("#shade").style.setProperty("--shade-height", `${100 - position}%`);
  document.querySelector("#position-readout").textContent = `Obiettivo · ${position}%`;
  positionButtons.forEach((button) => {
    button.setAttribute("aria-pressed", String(Number(button.dataset.position) === position));
  });
}

function renderDeviceMeta(device) {
  const meta = document.querySelector("#device-meta");
  meta.replaceChildren();
  for (const value of [
    device.display_name,
    device.current_ssid || "Wi-Fi non registrato",
    device.local_ip || "IP non registrato",
    device.last_seen ? `Visto ${new Date(device.last_seen).toLocaleString("it-IT")}` : "Mai connesso",
  ]) {
    const item = document.createElement("span");
    item.textContent = value;
    meta.append(item);
  }
}

async function loadDevice(deviceId) {
  const loadId = ++activeLoadId;
  activeDeviceId = deviceId;
  document.querySelector("#dashboard-error").textContent = "";
  setConnection("offline", "Connessione Realtime…");
  if (deviceChannel) await supabase.removeChannel(deviceChannel);
  if (loadId !== activeLoadId) return;

  const { data, error } = await supabase
    .from("devices")
    .select("device_id, display_name, current_ssid, local_ip, last_seen")
    .eq("device_id", deviceId)
    .single();
  if (loadId !== activeLoadId || deviceId !== activeDeviceId) return;
  if (error) {
    document.querySelector("#dashboard-error").textContent = error.message;
    return;
  }
  renderDeviceMeta(data);

  deviceChannel = supabase
    .channel(`device:${deviceId}`, { config: { private: true } })
    .on("broadcast", { event: "device_presence" }, ({ payload }) => {
      if (payload.device_id === activeDeviceId) renderDeviceMeta(payload);
    })
    .subscribe((status, error) => {
      if (loadId !== activeLoadId) return;
      if (status === "SUBSCRIBED") setConnection("live", "Canale Realtime pronto");
      else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT") {
        setConnection("error", "Realtime non disponibile");
        if (error) console.error("Realtime subscription error", error);
      }
    });
}

async function loadDevices() {
  const { data, error } = await supabase
    .from("devices")
    .select("device_id, display_name")
    .order("display_name");
  if (error) throw error;

  devicesSelect.replaceChildren();
  for (const device of data) {
    const option = document.createElement("option");
    option.value = device.device_id;
    option.textContent = device.display_name;
    devicesSelect.append(option);
  }
  if (data.length === 0) {
    document.querySelector("#dashboard-error").textContent = "Nessun dispositivo assegnato all'account.";
    return;
  }
  await loadDevice(data[0].device_id);
}

async function sendPosition(position) {
  if (!activeDeviceId) return;
  const needsTravelEstimate = position > 0 && position < 100;
  if (needsTravelEstimate && !validTravel()) {
    notice.dataset.state = "error";
    notice.textContent = "Inserisci un tempo tra 1 e 120 secondi.";
    travelInput.focus();
    return;
  }

  positionButtons.forEach((item) => item.disabled = true);
  notice.dataset.state = "";
  notice.textContent = `Registrazione obiettivo ${position}%…`;
  setPreview(position);
  const fullTravelMs = validTravel() ? Math.round(Number(travelInput.value) * 1000) : null;
  const { data, error } = await supabase.rpc("request_blind_position", {
    p_device_id: activeDeviceId,
    p_target_position: position,
    p_full_travel_ms: fullTravelMs,
  });
  positionButtons.forEach((item) => item.disabled = false);

  if (error) {
    notice.dataset.state = "error";
    notice.textContent = error.message;
  } else {
    localStorage.setItem("blind-full-travel-seconds", travelInput.value);
    notice.dataset.state = "ok";
    notice.textContent = `Comando ${position}% registrato · evento ${data}.`;
  }
}

async function showDashboard(session) {
  const nextUserId = session?.user?.id ?? null;
  if (nextUserId && nextUserId === activeUserId && !dashboard.hidden) return;
  activeUserId = nextUserId;
  loginPanel.hidden = Boolean(session);
  dashboard.hidden = !session;
  if (!session) {
    if (deviceChannel) await supabase.removeChannel(deviceChannel);
    deviceChannel = null;
    activeDeviceId = null;
    setConnection("offline", "Disconnesso");
    return;
  }

  setConnection("offline", "Connessione Realtime…");
  try {
    await loadDevices();
  } catch (error) {
    document.querySelector("#dashboard-error").textContent = error.message;
  }
}

document.querySelector("#login-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  const button = event.currentTarget.querySelector("button");
  button.disabled = true;
  document.querySelector("#login-error").textContent = "";
  const { data, error } = await supabase.auth.signInWithPassword({
    email: form.get("email"),
    password: form.get("password"),
  });
  button.disabled = false;
  if (error) {
    document.querySelector("#login-error").textContent = "Accesso non riuscito. Verifica email e password.";
    return;
  }
  await showDashboard(data.session);
});

devicesSelect.addEventListener("change", () => loadDevice(devicesSelect.value));
document.querySelector("#signout").addEventListener("click", async () => {
  await supabase.auth.signOut({ scope: "local" });
});
positionButtons.forEach((button) => button.addEventListener("click", () => {
  void sendPosition(Number(button.dataset.position));
}));
travelInput.addEventListener("change", () => {
  if (validTravel()) localStorage.setItem("blind-full-travel-seconds", travelInput.value);
});
supabase.auth.onAuthStateChange((_event, session) => {
  void showDashboard(session);
});