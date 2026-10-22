const corsHeaders = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, content-type",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};

function jsonResponse(status: number, body: Record<string, unknown>) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { ...corsHeaders, "Content-Type": "application/json" },
  });
}

function constantTimeEqual(left: string, right: string): boolean {
  if (left.length !== right.length) return false;
  let difference = 0;
  for (let index = 0; index < left.length; index++) {
    difference |= left.charCodeAt(index) ^ right.charCodeAt(index);
  }
  return difference === 0;
}

function isIPv4(value: string): boolean {
  const octets = value.split(".");
  return octets.length === 4 && octets.every((octet) => {
    if (!/^(0|[1-9]\d{0,2})$/.test(octet)) return false;
    return Number(octet) <= 255;
  });
}

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") {
    return new Response("ok", { headers: corsHeaders });
  }
  if (request.method !== "POST") {
    return jsonResponse(405, { error: "method_not_allowed" });
  }

  let payload: Record<string, unknown>;
  try {
    payload = await request.json();
  } catch {
    return jsonResponse(400, { error: "invalid_json" });
  }

  const deviceId = payload.device_id;
  const ssid = payload.ssid;
  const localIp = payload.local_ip;
  if (typeof deviceId !== "string" || !/^[A-Za-z0-9._-]{1,64}$/.test(deviceId) ||
      typeof ssid !== "string" || new TextEncoder().encode(ssid).length < 1 ||
      new TextEncoder().encode(ssid).length > 32 ||
      typeof localIp !== "string" || !isIPv4(localIp)) {
    return jsonResponse(400, { error: "invalid_device_record" });
  }

  let deviceTokens: Record<string, string>;
  try {
    deviceTokens = JSON.parse(Deno.env.get("DEVICE_REGISTRY_TOKENS") ?? "{}");
  } catch {
    return jsonResponse(500, { error: "server_configuration_error" });
  }

  const authorization = request.headers.get("Authorization") ?? "";
  const providedToken = authorization.startsWith("Bearer ")
    ? authorization.slice("Bearer ".length)
    : "";
  const expectedToken = deviceTokens[deviceId];
  if (typeof expectedToken !== "string" || !constantTimeEqual(providedToken, expectedToken)) {
    return jsonResponse(401, { error: "unauthorized_device" });
  }

  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const serviceRoleKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!supabaseUrl || !serviceRoleKey) {
    return jsonResponse(500, { error: "server_configuration_error" });
  }

  const insertResponse = await fetch(`${supabaseUrl}/rest/v1/device_network_history`, {
    method: "POST",
    headers: {
      apikey: serviceRoleKey,
      Authorization: `Bearer ${serviceRoleKey}`,
      "Content-Type": "application/json",
      Prefer: "return=minimal",
    },
    body: JSON.stringify({ device_id: deviceId, ssid, local_ip: localIp }),
  });

  if (!insertResponse.ok) {
    console.error("Device network insert failed with status", insertResponse.status);
    return jsonResponse(502, { error: "registry_insert_failed" });
  }

  const deviceUpdate = await fetch(
    `${supabaseUrl}/rest/v1/devices?device_id=eq.${encodeURIComponent(deviceId)}`,
    {
      method: "PATCH",
      headers: {
        apikey: serviceRoleKey,
        Authorization: `Bearer ${serviceRoleKey}`,
        "Content-Type": "application/json",
        Prefer: "return=minimal",
      },
      body: JSON.stringify({
        current_ssid: ssid,
        local_ip: localIp,
        last_seen: new Date().toISOString(),
      }),
    },
  );

  if (!deviceUpdate.ok) {
    console.error("Device presence update failed with status", deviceUpdate.status);
    return jsonResponse(502, { error: "device_presence_update_failed" });
  }

  return jsonResponse(201, { ok: true });
});