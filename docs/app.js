import { createClient } from "https://cdn.jsdelivr.net/npm/@supabase/supabase-js@2.91.0/+esm";
import { SUPABASE_PUBLISHABLE_KEY, SUPABASE_URL } from "./config.js";

const supabase = createClient(SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY);
const relayGrid = document.querySelector("#relay-grid");
const devicesSelect = document.querySelector("#device-select");
const loginPanel = document.querySelector("#login-panel");
const dashboard = document.querySelector("#dashboard");
const relayStates = new Map();
let activeDeviceId = null;
let activeUserId = null;
let activeDeviceLoad = 0;
let deviceChannel = null;

function setConnection(state, label) {
  const connection = document.querySelector("#connection");
  connection.dataset.state = state;
  document.querySelector("#connection-label").textContent = label;
}

function renderRelay(relayNumber) {
  const relay = relayStates.get(relayNumber);
  let button = relayGrid.querySelector(`[data-relay="${relayNumber}"]`);

  if (!button) {
    button = document.createElement("button");
    button.type = "button";
    button.className = "relay";
    button.dataset.relay = relayNumber;
    const name = relayNumber === 1 ? "Tapparella · salita"
      : relayNumber === 2 ? "Tapparella · discesa" : `Relè ${relayNumber}`;
    button.innerHTML = `<span class="relay-top"><span class="number">${String(relayNumber).padStart(2, "0")}</span><span class="switch" aria-hidden="true"></span></span><span class="name">${name}</span><span class="relay-state"></span>`;
    button.setAttribute("aria-disabled", String(relayNumber <= 2));
    if (relayNumber > 2) button.addEventListener("click", () => changeTarget(relayNumber));
    relayGrid.append(button);
  }

  const pending = relay.desired_state !== relay.actual_state;
  button.classList.toggle("on", relay.actual_state);
  button.classList.toggle("pending", pending);
  button.setAttribute("aria-pressed", String(relay.actual_state));
  button.querySelector(".relay-state").textContent = pending
    ? `IN ATTESA · ${relay.desired_state ? "ACCESO" : "SPENTO"}`
    : relay.actual_state ? "ACCESO" : "SPENTO";
}

function renderState(row) {
  if (row.device_id !== activeDeviceId) return;
  const relayNumber = Number(row.relay_number);
  if (relayNumber < 1 || relayNumber > 4) return;
  relayStates.set(relayNumber, {
    desired_state: Boolean(row.desired_state),
    actual_state: Boolean(row.actual_state),
  });
  renderRelay(relayNumber);
  document.querySelector("#updated-at").textContent = row.updated_at
    ? `Ultimo aggiornamento ${new Date(row.updated_at).toLocaleString("it-IT")}`
    : "";
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

async function loadDeviceSnapshot(deviceId, loadId) {
  const [deviceResult, stateResult] = await Promise.all([
    supabase
      .from("devices")
      .select("device_id, display_name, current_ssid, local_ip, last_seen")
      .eq("device_id", deviceId)
      .single(),
    supabase
      .from("relay_state")
      .select("device_id, relay_number, desired_state, actual_state, updated_at")
      .eq("device_id", deviceId)
      .order("relay_number"),
  ]);

  if (loadId !== activeDeviceLoad || deviceId !== activeDeviceId) return;
  if (deviceResult.error || stateResult.error) {
    document.querySelector("#dashboard-error").textContent =
      deviceResult.error?.message || stateResult.error?.message || "Lettura stato non riuscita";
    return;
  }

  renderDeviceMeta(deviceResult.data);
  stateResult.data.forEach(renderState);
}

async function loadDevice(deviceId) {
  const loadId = ++activeDeviceLoad;
  activeDeviceId = deviceId;
  relayStates.clear();
  relayGrid.replaceChildren();
  document.querySelector("#dashboard-error").textContent = "";
  document.querySelector("#updated-at").textContent = "";
  setConnection("offline", "Connessione Realtime…");
  if (deviceChannel) await supabase.removeChannel(deviceChannel);
  if (loadId !== activeDeviceLoad) return;

  deviceChannel = supabase
    .channel(`device:${deviceId}`, { config: { private: true } })
    .on("broadcast", { event: "relay_state" }, ({ payload }) => renderState(payload))
    .on("broadcast", { event: "device_presence" }, ({ payload }) => {
      if (payload.device_id !== activeDeviceId) return;
      renderDeviceMeta(payload);
    })
    .subscribe((status, error) => {
      if (loadId !== activeDeviceLoad) return;
      if (status === "SUBSCRIBED") {
        setConnection("live", "Realtime attivo");
        void loadDeviceSnapshot(deviceId, loadId);
      } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT") {
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

async function changeTarget(relayNumber) {
  const button = relayGrid.querySelector(`[data-relay="${relayNumber}"]`);
  const current = relayStates.get(relayNumber);
  if (!current || !activeDeviceId || button.disabled) return;

  button.disabled = true;
  const desiredState = !current.desired_state;
  const { error } = await supabase.rpc("set_relay_target", {
    p_device_id: activeDeviceId,
    p_relay_number: relayNumber,
    p_enabled: desiredState,
  });
  button.disabled = false;

  if (error) {
    document.querySelector("#dashboard-error").textContent = error.message;
    return;
  }

  renderState({
    device_id: activeDeviceId,
    relay_number: relayNumber,
    desired_state: desiredState,
    actual_state: current.actual_state,
    updated_at: new Date().toISOString(),
  });
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
supabase.auth.onAuthStateChange((_event, session) => {
  void showDashboard(session);
});