import { createClient } from "https://cdn.jsdelivr.net/npm/@supabase/supabase-js@2.91.0/+esm";
import { SUPABASE_PUBLISHABLE_KEY, SUPABASE_URL } from "./config.js";

const supabase = createClient(SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY);
const loginPanel = document.querySelector("#login-panel");
const dashboard = document.querySelector("#dashboard");
const notice = document.querySelector("#notice");
const actionButtons = [...document.querySelectorAll(".blind-action")];
let activeDeviceId = null;
let activeUserId = null;

async function loadDevices() {
  const { data, error } = await supabase
    .from("devices")
    .select("device_id, display_name")
    .order("display_name");
  if (error) throw error;

  if (data.length === 0) {
    document.querySelector("#dashboard-error").textContent = "Nessun dispositivo assegnato all'account.";
    return;
  }
  const preferredDeviceId = localStorage.getItem("relay-active-device-id");
  activeDeviceId = data.some((device) => device.device_id === preferredDeviceId)
    ? preferredDeviceId
    : data[0].device_id;
}

async function sendAction(action, button) {
  if (!activeDeviceId) return;
  actionButtons.forEach((item) => item.disabled = true);
  notice.dataset.state = "";
  notice.textContent = `Invio comando: ${button.querySelector("strong").textContent}…`;
  const { data, error } = await supabase.rpc("request_blind_action", {
    p_device_id: activeDeviceId,
    p_action: action,
  });
  actionButtons.forEach((item) => item.disabled = false);

  if (error) {
    notice.dataset.state = "error";
    notice.textContent = error.message;
  } else {
    notice.dataset.state = "ok";
    notice.textContent = `Comando registrato · evento ${data}.`;
  }
}

async function showDashboard(session) {
  const nextUserId = session?.user?.id ?? null;
  if (nextUserId && nextUserId === activeUserId && !dashboard.hidden) return;
  activeUserId = nextUserId;
  loginPanel.hidden = Boolean(session);
  dashboard.hidden = !session;
  if (!session) {
    activeDeviceId = null;
    return;
  }

  document.querySelector("#dashboard-error").textContent = "";
  try {
    await loadDevices();
    notice.textContent = activeDeviceId ? "Pronto" : "Nessun dispositivo disponibile";
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

actionButtons.forEach((button) => button.addEventListener("click", () => {
  void sendAction(button.dataset.action, button);
}));
supabase.auth.onAuthStateChange((_event, session) => {
  void showDashboard(session);
});