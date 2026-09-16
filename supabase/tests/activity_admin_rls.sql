-- Database authorization test for 20260916120000_activity_admin_writes.sql.
--
-- Run ONLY after that migration is applied, against the linked project:
--
--   supabase db query --linked -f supabase/tests/activity_admin_rls.sql
--
-- Scope (the rest is covered elsewhere, see PHASE6_PLAN §12.5):
--   * grants, policies, RLS flags and the single-admin index (catalog checks);
--   * `anon` as simulated by PostgREST (SET ROLE + request.jwt.claims):
--     SELECT allowed; INSERT, UPDATE, DELETE denied;
--   * a signed-in NON-admin: an arbitrary UUID in request.jwt.claims — no
--     auth.users row is created; SELECT allowed; INSERT denied; UPDATE and
--     DELETE change nothing; the allow-list is not readable or writable.
--   The public anon key is tested over REST by
--   scripts/check_activity_anon_access.ps1; the administrator write path is
--   tested through the web editor with the real account, after it exists.
--
-- Safety:
--   * one transaction ending in ROLLBACK; every failed check raises, which
--     aborts the transaction, so nothing is ever kept;
--   * refuses to run before the migration exists;
--   * inserts nothing into auth.users, the allow-list or activities (the
--     denied INSERTs are rejected before a row exists); UPDATE/DELETE
--     attempts are no-ops that RLS must filter out; TRUNCATE is never run
--     (checked from the grants only);
--   * no prompt, title, email, token or password is selected or printed.
--
-- Output on success: one row, "activity_admin_rls: all checks passed". Treat
-- any reported error as a failure.

begin;

do $guard$
begin
  if to_regprocedure('private.is_activity_admin()') is null
     or to_regclass('private.activity_admins') is null then
    raise exception 'activity_admin_rls: migration 20260916120000 is not applied; nothing was run';
  end if;
end
$guard$;

-- Baseline, as the migration owner: row count and a hash of every row.
create temporary table rls_test_baseline on commit drop as
select count(*) as n,
       md5(coalesce(string_agg(a::text, '|' order by a.id), '')) as h
  from public.activities a;

-- ---------------------------------------------------------- catalog checks

do $static$
declare
  priv text;
  missing text;
begin
  -- anon: SELECT only.
  foreach priv in array array['INSERT', 'UPDATE', 'DELETE', 'TRUNCATE', 'REFERENCES', 'TRIGGER'] loop
    if has_table_privilege('anon', 'public.activities', priv) then
      raise exception 'FAIL: anon has % on public.activities', priv;
    end if;
  end loop;
  if not has_table_privilege('anon', 'public.activities', 'SELECT') then
    raise exception 'FAIL: anon lost SELECT on public.activities';
  end if;

  -- authenticated: no TRUNCATE/REFERENCES/TRIGGER; writes stay RLS-gated.
  foreach priv in array array['TRUNCATE', 'REFERENCES', 'TRIGGER'] loop
    if has_table_privilege('authenticated', 'public.activities', priv) then
      raise exception 'FAIL: authenticated has % on public.activities', priv;
    end if;
  end loop;
  if not has_table_privilege('authenticated', 'public.activities', 'SELECT') then
    raise exception 'FAIL: authenticated lost SELECT on public.activities';
  end if;

  -- TRUNCATE also cannot come from PUBLIC.
  if exists (
    select 1
      from information_schema.role_table_grants
     where table_schema = 'public' and table_name = 'activities'
       and privilege_type = 'TRUNCATE'
       and grantee in ('anon', 'authenticated', 'PUBLIC')
  ) then
    raise exception 'FAIL: a TRUNCATE grant remains for anon/authenticated/PUBLIC';
  end if;

  -- Allow-list isolation.
  if has_schema_privilege('anon', 'private', 'USAGE') then
    raise exception 'FAIL: anon has USAGE on schema private';
  end if;
  foreach priv in array array['SELECT', 'INSERT', 'UPDATE', 'DELETE', 'TRUNCATE'] loop
    if has_table_privilege('anon', 'private.activity_admins', priv)
       or has_table_privilege('authenticated', 'private.activity_admins', priv) then
      raise exception 'FAIL: the allow-list grants % to anon/authenticated', priv;
    end if;
  end loop;
  if has_function_privilege('anon', 'private.is_activity_admin()', 'EXECUTE') then
    raise exception 'FAIL: anon can execute private.is_activity_admin()';
  end if;
  if not has_function_privilege('authenticated', 'private.is_activity_admin()', 'EXECUTE') then
    raise exception 'FAIL: authenticated cannot execute private.is_activity_admin()';
  end if;
  if not (select p.prosecdef from pg_proc p
           where p.oid = 'private.is_activity_admin()'::regprocedure) then
    raise exception 'FAIL: private.is_activity_admin() is not SECURITY DEFINER';
  end if;

  -- RLS flags.
  if not (select relrowsecurity from pg_class
           where oid = 'private.activity_admins'::regclass) then
    raise exception 'FAIL: RLS is not enabled on private.activity_admins';
  end if;
  if exists (select 1 from pg_policies
              where schemaname = 'private' and tablename = 'activity_admins') then
    raise exception 'FAIL: private.activity_admins has policies';
  end if;
  if not (select relrowsecurity from pg_class
           where oid = 'public.activities'::regclass) then
    raise exception 'FAIL: RLS is not enabled on public.activities';
  end if;

  -- At most one administrator: a unique index on a constant expression.
  if not exists (
    select 1
      from pg_index i
     where i.indrelid = 'private.activity_admins'::regclass
       and i.indisunique
       and i.indexprs is not null
       and pg_get_indexdef(i.indexrelid) like '%(true)%'
  ) then
    raise exception 'FAIL: the single-administrator unique index is missing';
  end if;
  if (select count(*) from private.activity_admins) > 1 then
    raise exception 'FAIL: more than one administrator is enrolled';
  end if;

  -- Exactly the expected policies on public.activities.
  select string_agg(expected.policyname, ', ')
    into missing
    from (
      values
        ('anon can read activities', 'SELECT', '{anon}'),
        ('authenticated can read activities', 'SELECT', '{authenticated}'),
        ('admin can insert activities', 'INSERT', '{authenticated}'),
        ('admin can update activities', 'UPDATE', '{authenticated}'),
        ('admin can delete activities', 'DELETE', '{authenticated}')
    ) as expected(policyname, cmd, roles)
   where not exists (
     select 1 from pg_policies p
      where p.schemaname = 'public' and p.tablename = 'activities'
        and p.policyname = expected.policyname
        and p.cmd = expected.cmd
        and p.roles::text = expected.roles
   );
  if missing is not null then
    raise exception 'FAIL: missing or different policies: %', missing;
  end if;
  if (select count(*) from pg_policies
       where schemaname = 'public' and tablename = 'activities') <> 5 then
    raise exception 'FAIL: unexpected extra policies on public.activities';
  end if;
end
$static$;

-- ------------------------------------------------------------------- anon

do $anon$
declare
  n bigint;
  expected bigint := (select b.n from rls_test_baseline b);
begin
  set local role anon;
  perform set_config('request.jwt.claims', '{"role":"anon"}', true);

  select count(*) into n from public.activities;
  if n <> expected then
    raise exception 'FAIL: anon SELECT does not see every activity';
  end if;

  begin
    insert into public.activities (title, type, prompt, interaction_mode, enabled)
    values ('rls-test anon (must be denied)', 'lesson', 'x', 'push_to_talk', false);
    raise exception 'FAIL: anon INSERT was allowed';
  exception when insufficient_privilege then null;
  end;

  begin
    update public.activities set sort_order = sort_order;
    raise exception 'FAIL: anon UPDATE was allowed';
  exception when insufficient_privilege then null;
  end;

  begin
    delete from public.activities;
    raise exception 'FAIL: anon DELETE was allowed';
  exception when insufficient_privilege then null;
  end;

  begin
    perform 1 from private.activity_admins;
    raise exception 'FAIL: anon can read the allow-list';
  exception when insufficient_privilege then null;
  end;

  begin
    perform private.is_activity_admin();
    raise exception 'FAIL: anon can call private.is_activity_admin()';
  exception when insufficient_privilege then null;
  end;

  reset role;
end
$anon$;

-- ------------------------------ authenticated, NOT an administrator
-- An arbitrary UUID that is not (and cannot be) in the allow-list; no
-- auth.users row is needed for RLS evaluation.

do $user$
declare
  n bigint;
  expected bigint := (select b.n from rls_test_baseline b);
begin
  set local role authenticated;
  perform set_config(
    'request.jwt.claims',
    '{"sub":"7f3c2b1a-0d4e-4c5f-9a8b-000000000000","role":"authenticated","aud":"authenticated"}',
    true
  );

  if private.is_activity_admin() then
    raise exception 'FAIL: a non-listed identity is reported as admin';
  end if;

  select count(*) into n from public.activities;
  if n <> expected then
    raise exception 'FAIL: authenticated SELECT does not see every activity';
  end if;

  begin
    insert into public.activities (title, type, prompt, interaction_mode, enabled)
    values ('rls-test non-admin (must be denied)', 'lesson', 'x', 'push_to_talk', false);
    raise exception 'FAIL: non-admin INSERT was allowed';
  exception when insufficient_privilege then null;
  end;

  -- No-op updates/deletes over every row: RLS must filter all of them out.
  update public.activities set sort_order = sort_order;
  get diagnostics n = row_count;
  if n <> 0 then
    raise exception 'FAIL: non-admin UPDATE matched % row(s)', n;
  end if;

  delete from public.activities;
  get diagnostics n = row_count;
  if n <> 0 then
    raise exception 'FAIL: non-admin DELETE matched % row(s)', n;
  end if;

  begin
    perform 1 from private.activity_admins;
    raise exception 'FAIL: authenticated can read the allow-list';
  exception when insufficient_privilege then null;
  end;

  begin
    insert into private.activity_admins (user_id)
    values ('7f3c2b1a-0d4e-4c5f-9a8b-000000000000');
    raise exception 'FAIL: authenticated can write the allow-list';
  exception when insufficient_privilege then null;
  end;

  reset role;
end
$user$;

-- ------------------------------------------- existing activities unchanged

do $unchanged$
begin
  if (select md5(coalesce(string_agg(a::text, '|' order by a.id), ''))
        from public.activities a)
     is distinct from (select h from rls_test_baseline) then
    raise exception 'FAIL: activities changed during the test';
  end if;
end
$unchanged$;

rollback;

select 'activity_admin_rls: all checks passed' as result;
