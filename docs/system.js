import { createClient } from "https://cdn.jsdelivr.net/npm/@supabase/supabase-js@2.91.0/+esm";
import { SUPABASE_PUBLISHABLE_KEY, SUPABASE_URL } from "./config.js";

const supabase = createClient(SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY);
const loginPanel = document.querySelector("#login-panel");
const dashboard = document.querySelector("#dashboard");
const deviceList = document.querySelector("#device-list");
let activeUserId = null;

function renderDevices(devices) {
  deviceList.replaceChildren();
  if (devices.length === 0) {
    const empty = document.createElement("p");
    empty.className = "empty";
    empty.textContent = "Nessun dispositivo registrato nell'account.";
    deviceList.append(empty);
    return;
  }

  for (const device of devices) {
    const item = document.createElement("article");
    item.className = "device";
    const name = document.createElement("div");
    name.className = "device-name";
    name.textContent = device.display_name;
    const presence = document.createElement("div");
    presence.className = "presence";
    const isOnline = device.last_seen && Date.now() - new Date(device.last_seen).getTime() < 120000;
    presence.dataset.online = String(Boolean(isOnline));
    presence.textContent = isOnline ? "Attivo" : device.last_seen ? "Non recente" : "Mai visto";
    const id = document.createElement("div");
    id.className = "device-id";
    id.textContent = device.device_id;
    const meta = document.createElement("div");
    meta.className = "device-meta";
    for (const text of [
      device.current_ssid || "Wi-Fi non registrato",
      device.local_ip || "IP non registrato",
      device.last_seen ? `Ultimo contatto ${new Date(device.last_seen).toLocaleString("it-IT")}` : "",
    ].filter(Boolean)) {
      const value = document.createElement("span");
      value.textContent = text;
      meta.append(value);
    }
    item.append(name, presence, id, meta);
    deviceList.append(item);
  }
}

async function loadDevices() {
  document.querySelector("#dashboard-error").textContent = "";
  const { data, error } = await supabase
    .from("devices")
    .select("device_id, display_name, current_ssid, local_ip, last_seen")
    .eq("owner_user_id", activeUserId)
    .order("display_name");
  if (error) {
    document.querySelector("#dashboard-error").textContent = error.message;
    return;
  }
  document.querySelector("#device-count").textContent =
    data.length === 1 ? "1 dispositivo" : `${data.length} dispositivi`;
  renderDevices(data);
}

async function showDashboard(session) {
  const nextUserId = session?.user?.id ?? null;
  if (nextUserId && nextUserId === activeUserId && !dashboard.hidden) return;
  activeUserId = nextUserId;
  loginPanel.hidden = Boolean(session);
  dashboard.hidden = !session;
  if (session) await loadDevices();
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

document.querySelector("#signout").addEventListener("click", async () => {
  await supabase.auth.signOut({ scope: "local" });
});

supabase.auth.onAuthStateChange((_event, session) => {
  void showDashboard(session);
});