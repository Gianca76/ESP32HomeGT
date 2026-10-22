create table if not exists public.device_network_history (
  id bigint generated always as identity primary key,
  device_id text not null,
  ssid text not null,
  local_ip inet not null,
  created_at timestamptz not null default now()
);

create index if not exists device_network_history_device_created_idx
  on public.device_network_history (device_id, created_at desc);

alter table public.device_network_history enable row level security;
revoke all on public.device_network_history from anon, authenticated;
grant all on public.device_network_history to service_role;
grant usage, select on sequence public.device_network_history_id_seq to service_role;