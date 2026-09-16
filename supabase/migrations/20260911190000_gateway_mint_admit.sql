-- Phase 6 (PHASE6_PLAN §4.3): replay store + rate limit for the
-- `gateway-gemini-token` Edge Function.
--
-- One atomic admission step per mint request:
--   * a nonce may be used once (replay protection for the HMAC assertion);
--   * at most p_limit mints per p_window_seconds (rate limit).
--
-- Only `service_role` can execute it (the Edge Function calls it with the
-- service key the Edge runtime provides). anon / authenticated have no
-- access to the function or the table. Additive only: nothing existing is
-- changed, so applying this is safe for the Flutter app.

create table if not exists public.gateway_mint_nonces (
  nonce text primary key,
  created_at timestamptz not null default now()
);

create index if not exists gateway_mint_nonces_created_at_idx
  on public.gateway_mint_nonces (created_at);

-- RLS on with NO policies: nothing but service_role (which bypasses RLS) and
-- the SECURITY DEFINER function below can read or write it.
alter table public.gateway_mint_nonces enable row level security;
revoke all on public.gateway_mint_nonces from anon, authenticated;

create or replace function public.gateway_mint_admit(
  p_nonce text,
  p_limit integer,
  p_window_seconds integer
)
returns text
language plpgsql
security definer
set search_path = public
as $$
declare
  recent integer;
begin
  -- 16 random bytes, base64url: exactly 22 characters.
  if p_nonce is null or p_nonce !~ '^[A-Za-z0-9_-]{22}$' then
    return 'replay';
  end if;
  if p_limit is null or p_limit < 1 or p_window_seconds is null or p_window_seconds < 1 then
    return 'rate_limited';
  end if;

  -- Serialise admissions so the count and the insert are exact under
  -- concurrent requests.
  perform pg_advisory_xact_lock(hashtext('gateway_mint_admit'));

  -- Nonces only need to outlive the ±60 s timestamp window; keep 10 min.
  delete from public.gateway_mint_nonces
   where created_at < now() - interval '10 minutes';

  select count(*) into recent
    from public.gateway_mint_nonces
   where created_at > now() - make_interval(secs => p_window_seconds);
  if recent >= p_limit then
    return 'rate_limited';
  end if;

  insert into public.gateway_mint_nonces (nonce) values (p_nonce)
  on conflict (nonce) do nothing;
  if not found then
    return 'replay';
  end if;
  return 'ok';
end;
$$;

revoke all on function public.gateway_mint_admit(text, integer, integer)
  from public, anon, authenticated;
grant execute on function public.gateway_mint_admit(text, integer, integer)
  to service_role;
