create table if not exists public.blind_commands (
  command_id bigint generated always as identity primary key,
  device_id text not null references public.devices(device_id) on delete cascade,
  target_position smallint not null check (target_position between 0 and 100),
  full_travel_ms integer check (full_travel_ms between 1000 and 120000),
  created_at timestamptz not null default now()
);

create index if not exists blind_commands_device_created_idx
  on public.blind_commands (device_id, created_at desc);

alter table public.blind_commands enable row level security;
revoke all on public.blind_commands from anon, authenticated;
grant select on public.blind_commands to authenticated;
grant all on public.blind_commands to service_role;

drop policy if exists "Owners can read blind command history" on public.blind_commands;
create policy "Owners can read blind command history"
  on public.blind_commands for select to authenticated
  using (
    exists (
      select 1 from public.devices d
      where d.device_id = blind_commands.device_id
        and d.owner_user_id = (select auth.uid())
    )
  );

create or replace function public.request_blind_position(
  p_device_id text,
  p_target_position smallint,
  p_full_travel_ms integer
)
returns bigint
language plpgsql
security definer
set search_path = ''
as $$
declare
  new_command_id bigint;
begin
  if auth.uid() is null or not exists (
    select 1 from public.devices d
    where d.device_id = p_device_id and d.owner_user_id = auth.uid()
  ) then
    raise insufficient_privilege;
  end if;

    if p_target_position is null or p_target_position not between 0 and 100
      or (p_target_position not in (0, 100) and
        (p_full_travel_ms is null or p_full_travel_ms not between 1000 and 120000))
      or (p_full_travel_ms is not null and p_full_travel_ms not between 1000 and 120000) then
    raise invalid_parameter_value;
  end if;

  insert into public.blind_commands (device_id, target_position, full_travel_ms)
  values (p_device_id, p_target_position, p_full_travel_ms)
  returning command_id into new_command_id;

  return new_command_id;
end;
$$;

revoke all on function public.request_blind_position(text, smallint, integer)
  from public, anon, authenticated;
grant execute on function public.request_blind_position(text, smallint, integer)
  to authenticated;

create or replace function public.get_recent_blind_command(p_device_id text)
returns table(command_id bigint, target_position smallint, full_travel_ms integer)
language plpgsql
security definer
set search_path = ''
as $$
begin
  if auth.uid() is null or not exists (
    select 1 from public.devices d
    where d.device_id = p_device_id and d.auth_user_id = auth.uid()
  ) then
    raise insufficient_privilege;
  end if;

  return query
  select c.command_id, c.target_position, c.full_travel_ms
  from public.blind_commands c
  where c.device_id = p_device_id
    and c.created_at >= now() - interval '5 minutes'
  order by c.command_id desc
  limit 1;
end;
$$;

revoke all on function public.get_recent_blind_command(text)
  from public, anon, authenticated;
grant execute on function public.get_recent_blind_command(text) to authenticated;

create or replace function public.broadcast_blind_command()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  perform realtime.send(
    jsonb_build_object(
      'device_id', new.device_id,
      'command_id', new.command_id,
      'target_position', new.target_position,
      'full_travel_ms', new.full_travel_ms,
      'created_at', new.created_at
    ),
    'blind_command',
    'device:' || new.device_id,
    true
  );
  return new;
end;
$$;

drop trigger if exists broadcast_blind_command_insert on public.blind_commands;
create trigger broadcast_blind_command_insert
  after insert on public.blind_commands
  for each row execute function public.broadcast_blind_command();

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

  if p_relay_number not between 3 and 4 then
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

revoke all on function public.set_relay_target(text, smallint, boolean)
  from public, anon, authenticated;
grant execute on function public.set_relay_target(text, smallint, boolean) to authenticated;