-- Phase 6 (PHASE6_PLAN §4.4): prompt mutation requires an authorised role.
--
-- STATUS: PENDING — deliberately kept OUT of supabase/migrations so that a
-- routine `supabase db push` cannot apply it early.
--
-- Why: the gateway turns activity prompts into Gemini SYSTEM INSTRUCTIONS.
-- Today `anon` (the publishable key, embedded in the apps) can insert,
-- update and delete activities, so anyone holding that key could rewrite
-- what the robot is instructed to do.
--
-- What it does:
--   * keeps anonymous READ (the Android app and the gateway read activities);
--   * removes anonymous insert / update / delete;
--   * allows writes only to authenticated users whose app_metadata.role is
--     'activity_editor' (set by an admin with the service role). A plain
--     sign-up is NOT enough: config.toml has enable_signup = true, so
--     "authenticated" alone would let anyone write.
--
-- Consequence: the Flutter web activity form writes as anon today, so once
-- this is applied web editing stops until the web app signs in as an editor
-- (a separately reviewed Flutter change). Reads and the Android app are
-- unaffected, so applying it early is fail-safe.
--
-- GATE: the gateway must not be deployed publicly before this is applied.
-- To apply: move this file into supabase/migrations/ and `supabase db push`.

drop policy if exists "anon can insert activities" on public.activities;
drop policy if exists "anon can update activities" on public.activities;
drop policy if exists "anon can delete activities" on public.activities;

create policy "activity editors can insert activities"
  on public.activities
  for insert
  to authenticated
  with check ((auth.jwt() -> 'app_metadata' ->> 'role') = 'activity_editor');

create policy "activity editors can update activities"
  on public.activities
  for update
  to authenticated
  using ((auth.jwt() -> 'app_metadata' ->> 'role') = 'activity_editor')
  with check ((auth.jwt() -> 'app_metadata' ->> 'role') = 'activity_editor');

create policy "activity editors can delete activities"
  on public.activities
  for delete
  to authenticated
  using ((auth.jwt() -> 'app_metadata' ->> 'role') = 'activity_editor');

-- Unchanged: "anon can read activities" (select to anon using (true)).
