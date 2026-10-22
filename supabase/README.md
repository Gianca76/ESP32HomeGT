# Supabase setup

## Apply schema

From the repository root, link the project if needed and apply the migrations:

```powershell
npx supabase link --project-ref <project-ref>
npx supabase db push
npx supabase functions deploy register-device --no-verify-jwt
```

The migrations protect the exposed tables with RLS. Keep **Realtime → Settings → Allow public access** disabled so `device:<device_id>` requires a signed-in user.

## Create users and assign a device

Create the human owner and one confirmed email/password Auth user per ESP32 in the Supabase dashboard. Use the device Auth user's email/password only in that board's ignored `include/secrets.h` as `DEVICE_AUTH_EMAIL` and `DEVICE_AUTH_PASSWORD`. Keep `DEVICE_REGISTRY_TOKEN` separate; it authenticates network-history registration.

In the SQL Editor, associate the two Auth users with the device. Replace the example ID and emails:

```sql
insert into public.devices (device_id, display_name, owner_user_id, auth_user_id)
select 'esp32-relay-01', 'Relay Studio', owner.id, device.id
from auth.users as owner
join auth.users as device
  on device.email = 'esp32-relay-01@devices.local'
where owner.email = 'YOUR_OWNER_EMAIL'
on conflict (device_id) do update
set display_name = excluded.display_name,
    owner_user_id = excluded.owner_user_id,
    auth_user_id = excluded.auth_user_id;

insert into public.relay_state (device_id, relay_number)
select 'esp32-relay-01', relays.relay_number
from generate_series(1, 4) as relays(relay_number)
on conflict (device_id, relay_number) do nothing;
```

Add each device's `DEVICE_ID` and `DEVICE_REGISTRY_TOKEN` to the server-side `DEVICE_REGISTRY_TOKENS` JSON secret. Never put a Supabase secret/service-role key in `secrets.h` or `docs/`.

## GitHub Pages

`docs/config.js` contains the project URL and **publishable** key, which is intended to be public. RLS is the authorization boundary. In the repository, select **Settings → Pages → GitHub Actions**; pushes to `main` that change `docs/` deploy the remote dashboard. The dashboard login uses the human owner Auth account.

The LAN page in `data/` remains served by the ESP32 and does not require Supabase. To update it, upload LittleFS separately with PlatformIO `uploadfs`.