alter table public.blind_commands
  add column if not exists action text;

alter table public.blind_commands
  alter column target_position drop not null;

alter table public.blind_commands
  drop constraint if exists blind_commands_action_check;
alter table public.blind_commands
  add constraint blind_commands_action_check
  check (action is null or action in ('down', 'up_short', 'up_half', 'up'));

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
begin
  raise invalid_parameter_value using message = 'Use request_blind_action';
end;
$$;

create or replace function public.request_blind_action(
  p_device_id text,
  p_action text
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

  if p_action is null or p_action not in ('down', 'up_short', 'up_half', 'up') then
    raise invalid_parameter_value;
  end if;

  insert into public.blind_commands (device_id, action)
  values (p_device_id, p_action)
  returning command_id into new_command_id;

  return new_command_id;
end;
$$;

revoke all on function public.request_blind_action(text, text)
  from public, anon, authenticated;
grant execute on function public.request_blind_action(text, text)
  to authenticated;

drop function if exists public.get_recent_blind_command(text);
create function public.get_recent_blind_command(p_device_id text)
returns table(command_id bigint, action text)
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
  select c.command_id, c.action
  from public.blind_commands c
  where c.device_id = p_device_id
    and c.action is not null
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
      'action', new.action,
      'created_at', new.created_at
    ),
    'blind_command',
    'device:' || new.device_id,
    true
  );
  return new;
end;
$$;

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
grant execute on function public.set_relay_target(text, smallint, boolean)
  to authenticated;