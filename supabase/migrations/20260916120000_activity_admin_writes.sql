-- Activity writes only by the single approved administrator.
--
-- Replaces supabase/pending/20260911190100_restrict_activity_writes.sql
-- (deleted), which granted writes to any authenticated user carrying an
-- app_metadata role, added no SELECT for signed-in users and left the broad
-- table grants (including TRUNCATE, which RLS does not cover) in place.
--
-- After this migration:
--   anon           SELECT only (the gateway and the Android app read with the
--                  publishable key);
--   authenticated  SELECT for everyone signed in; INSERT / UPDATE / DELETE
--                  only when auth.uid() is in private.activity_admins;
--   service_role   unchanged (bypasses RLS; Dashboard/administration keeps
--                  working).
--
-- The allow-list lives in schema `private`, which the Supabase Data API does
-- not expose, has no grants for anon/authenticated and holds at most one row.
-- The administrator is added separately, after the account exists (see
-- firmware/core2/docs/PHASE6_PLAN.md §12.5) — never in a migration.
--
-- No activity row is read, changed or deleted by this migration.

-- ---------------------------------------------------------------- allow-list

create schema if not exists private;
revoke all on schema private from public, anon, authenticated;
-- The write policies below call private.is_activity_admin() as the
-- requesting role, which needs USAGE on the schema (not on the table).
grant usage on schema private to authenticated;

create table if not exists private.activity_admins (
  user_id uuid primary key references auth.users (id) on delete cascade,
  created_at timestamptz not null default now()
);

-- At most ONE administrator: every row indexes the same constant value.
create unique index if not exists activity_admins_single_admin
  on private.activity_admins ((true));

-- RLS on with no policies: only the table owner, service_role and the
-- SECURITY DEFINER function below can see the rows.
alter table private.activity_admins enable row level security;
revoke all on private.activity_admins from public, anon, authenticated;

create or replace function private.is_activity_admin()
returns boolean
language sql
stable
security definer
set search_path = ''
as $$
  select exists (
    select 1
      from private.activity_admins
     where user_id = (select auth.uid())
  );
$$;

revoke all on function private.is_activity_admin() from public, anon;
grant execute on function private.is_activity_admin() to authenticated;

-- ---------------------------------------------------------------- policies

drop policy if exists "anon can insert activities" on public.activities;
drop policy if exists "anon can update activities" on public.activities;
drop policy if exists "anon can delete activities" on public.activities;
-- Kept unchanged: "anon can read activities" (select to anon using (true)).

drop policy if exists "authenticated can read activities" on public.activities;
create policy "authenticated can read activities"
  on public.activities
  for select
  to authenticated
  using (true);

drop policy if exists "admin can insert activities" on public.activities;
create policy "admin can insert activities"
  on public.activities
  for insert
  to authenticated
  with check ((select private.is_activity_admin()));

drop policy if exists "admin can update activities" on public.activities;
create policy "admin can update activities"
  on public.activities
  for update
  to authenticated
  using ((select private.is_activity_admin()))
  with check ((select private.is_activity_admin()));

drop policy if exists "admin can delete activities" on public.activities;
create policy "admin can delete activities"
  on public.activities
  for delete
  to authenticated
  using ((select private.is_activity_admin()));

-- ---------------------------------------------------------------- grants

-- RLS does not apply to TRUNCATE, so table privileges must be narrowed too.
revoke insert, update, delete, truncate, references, trigger
  on public.activities from anon;
revoke truncate, references, trigger
  on public.activities from authenticated;
-- Remaining: anon SELECT; authenticated SELECT, INSERT, UPDATE, DELETE (all
-- gated by the policies above); service_role unchanged.
