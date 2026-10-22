create table if not exists public.devices (
  device_id text primary key check (device_id ~ '^[A-Za-z0-9._-]{1,64}$'),
  display_name text not null,
  owner_user_id uuid not null references auth.users(id) on delete cascade,
  auth_user_id uuid not null unique references auth.users(id) on delete cascade,
  current_ssid text,
  local_ip inet,
  last_seen timestamptz,
  created_at timestamptz not null default now()
);

create table if not exists public.relay_state (
  device_id text not null references public.devices(device_id) on delete cascade,
  relay_number smallint not null check (relay_number between 1 and 4),
  desired_state boolean not null default false,
  actual_state boolean not null default false,
  desired_changed_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  primary key (device_id, relay_number)
);

create table if not exists public.relay_history (
  id bigint generated always as identity primary key,
  device_id text not null references public.devices(device_id) on delete cascade,
  relay_number smallint not null check (relay_number between 1 and 4),
  state boolean not null,
  source text not null check (source in ('local', 'remote', 'reconcile')),
  created_at timestamptz not null default now()
);

create index if not exists relay_history_device_created_idx
  on public.relay_history (device_id, created_at desc);

alter table public.devices enable row level security;
alter table public.relay_state enable row level security;
alter table public.relay_history enable row level security;

revoke all on public.devices, public.relay_state, public.relay_history
  from anon, authenticated;
grant select on public.devices, public.relay_state, public.relay_history
  to authenticated;
grant all on public.devices, public.relay_state, public.relay_history
  to service_role;
grant usage, select on sequence public.relay_history_id_seq to service_role;

drop policy if exists "Owners and device users can read devices" on public.devices;
create policy "Owners and device users can read devices"
  on public.devices for select to authenticated
  using (owner_user_id = (select auth.uid()) or auth_user_id = (select auth.uid()));

drop policy if exists "Owners and device users can read relay state" on public.relay_state;
create policy "Owners and device users can read relay state"
  on public.relay_state for select to authenticated
  using (
    exists (
      select 1 from public.devices d
      where d.device_id = relay_state.device_id
        and (d.owner_user_id = (select auth.uid()) or d.auth_user_id = (select auth.uid()))
    )
  );

drop policy if exists "Owners can read relay history" on public.relay_history;
create policy "Owners can read relay history"
  on public.relay_history for select to authenticated
  using (
    exists (
      select 1 from public.devices d
      where d.device_id = relay_history.device_id
        and d.owner_user_id = (select auth.uid())
    )
  );

create or replace function public.set_relay_target(
  p_device_id text,
  p_relay_number smallint,
  p_enabled boolean
)
returns void
language plpgsql
security definer
set search_path = ''
as $$
begin
  if auth.uid() is null or not exists (
    select 1 from public.devices d
    where d.device_id = p_device_id and d.owner_user_id = auth.uid()
  ) then
    raise insufficient_privilege;
  end if;

  if p_relay_number not between 1 and 4 then
    raise invalid_parameter_value;
  end if;

  update public.relay_state
  set desired_state = p_enabled,
      desired_changed_at = now()
  where device_id = p_device_id
    and relay_number = p_relay_number
    and desired_state is distinct from p_enabled;

  if not found and not exists (
    select 1 from public.relay_state
    where device_id = p_device_id and relay_number = p_relay_number
  ) then
    raise no_data_found;
  end if;
end;
$$;

create or replace function public.report_relay_state(
  p_device_id text,
  p_relay_number smallint,
  p_enabled boolean,
  p_source text
)
returns void
language plpgsql
security definer
set search_path = ''
as $$
declare
  current_state public.relay_state%rowtype;
begin
  if auth.uid() is null or not exists (
    select 1 from public.devices d
    where d.device_id = p_device_id and d.auth_user_id = auth.uid()
  ) then
    raise insufficient_privilege;
  end if;

    if p_relay_number not between 1 and 4 or p_source is null
      or p_source not in ('local', 'remote', 'reconcile') then
    raise invalid_parameter_value;
  end if;

  select * into current_state
  from public.relay_state
  where device_id = p_device_id and relay_number = p_relay_number
  for update;

  if not found then
    raise no_data_found;
  end if;

  if current_state.actual_state is distinct from p_enabled then
    insert into public.relay_history (device_id, relay_number, state, source)
    values (p_device_id, p_relay_number, p_enabled, p_source);
  end if;

  update public.relay_state
  set actual_state = p_enabled,
      desired_state = case when p_source = 'local' then p_enabled else desired_state end,
      desired_changed_at = case when p_source = 'local' then now() else desired_changed_at end,
      updated_at = now()
  where device_id = p_device_id and relay_number = p_relay_number;
end;
$$;

revoke all on function public.set_relay_target(text, smallint, boolean)
  from public, anon, authenticated;
revoke all on function public.report_relay_state(text, smallint, boolean, text)
  from public, anon, authenticated;
grant execute on function public.set_relay_target(text, smallint, boolean) to authenticated;
grant execute on function public.report_relay_state(text, smallint, boolean, text) to authenticated;

create or replace function public.broadcast_relay_state()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  if old.desired_state is not distinct from new.desired_state
     and old.actual_state is not distinct from new.actual_state then
    return new;
  end if;

  perform realtime.send(
    jsonb_build_object(
      'device_id', new.device_id,
      'relay_number', new.relay_number,
      'desired_state', new.desired_state,
      'actual_state', new.actual_state,
      'desired_changed_at', new.desired_changed_at,
      'updated_at', new.updated_at
    ),
    'relay_state',
    'device:' || new.device_id,
    true
  );
  return new;
end;
$$;

drop trigger if exists broadcast_relay_state_change on public.relay_state;
create trigger broadcast_relay_state_change
  after update of desired_state, actual_state on public.relay_state
  for each row execute function public.broadcast_relay_state();

create or replace function public.broadcast_device_presence()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  if old.current_ssid is not distinct from new.current_ssid
     and old.local_ip is not distinct from new.local_ip
     and old.last_seen is not distinct from new.last_seen then
    return new;
  end if;

  perform realtime.send(
    jsonb_build_object(
      'device_id', new.device_id,
      'display_name', new.display_name,
      'current_ssid', new.current_ssid,
      'local_ip', new.local_ip,
      'last_seen', new.last_seen
    ),
    'device_presence',
    'device:' || new.device_id,
    true
  );
  return new;
end;
$$;

drop trigger if exists broadcast_device_presence_change on public.devices;
create trigger broadcast_device_presence_change
  after update of current_ssid, local_ip, last_seen on public.devices
  for each row execute function public.broadcast_device_presence();

drop policy if exists "Assigned users can receive private device broadcasts" on realtime.messages;
create policy "Assigned users can receive private device broadcasts"
  on realtime.messages for select to authenticated
  using (
    extension = 'broadcast'
    and exists (
      select 1 from public.devices d
      where realtime.topic() = 'device:' || d.device_id
        and (d.owner_user_id = (select auth.uid()) or d.auth_user_id = (select auth.uid()))
    )
  );

grant select on public.device_network_history to authenticated;
drop policy if exists "Owners can read device network history" on public.device_network_history;
create policy "Owners can read device network history"
  on public.device_network_history for select to authenticated
  using (
    exists (
      select 1 from public.devices d
      where d.device_id = device_network_history.device_id
        and d.owner_user_id = (select auth.uid())
    )
  );