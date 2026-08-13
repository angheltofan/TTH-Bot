-- EduBot V1: activities table.
--
-- Deliberately minimal: one table, no auth, no groups/classes. RLS is
-- enabled but the pilot phase grants full CRUD to the anonymous/publishable
-- client role — see the policies block below for why, and what must change
-- before this configuration interface is exposed outside a controlled pilot.

create table if not exists public.activities (
  id uuid primary key default gen_random_uuid(),
  title text not null,
  type text not null check (type in ('lesson', 'conversation', 'game')),
  prompt text not null,
  participants text[] not null default '{}',
  interaction_mode text not null check (
    interaction_mode in ('push_to_talk', 'free_conversation')
  ),
  enabled boolean not null default true,
  sort_order integer not null default 0,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

-- The Android selector's main query is "enabled activities, in sort order".
create index if not exists activities_enabled_sort_order_idx
  on public.activities (enabled, sort_order);

-- Keep updated_at accurate automatically instead of relying on every caller
-- (Flutter web form, future scripts, manual SQL edits) to set it correctly.
create or replace function public.set_updated_at()
returns trigger
language plpgsql
as $$
begin
  new.updated_at = now();
  return new;
end;
$$;

drop trigger if exists activities_set_updated_at on public.activities;
create trigger activities_set_updated_at
  before update on public.activities
  for each row
  execute function public.set_updated_at();

-- Row Level Security
--
-- EduBot V1 has no user accounts of any kind (see task scope: no auth, no
-- admin/trainer roles). The publishable client key authenticates as
-- Postgres role `anon`, the same low-privilege role the legacy anon key
-- used — so these policies apply equally whether the app is on legacy or
-- new API keys.
--
-- IMPORTANT: granting anon full read/write here is acceptable ONLY because
-- this is a controlled V1 pilot with a small, trusted group of testers and
-- no public URL for the web configuration interface. Before opening the
-- web interface (or the Supabase project) to anyone outside that pilot,
-- this must be hardened — at minimum with Supabase Auth-gated write access
-- for the web app, keeping reads anonymous for the Android app.
alter table public.activities enable row level security;

create policy "anon can read activities"
  on public.activities
  for select
  to anon
  using (true);

create policy "anon can insert activities"
  on public.activities
  for insert
  to anon
  with check (true);

create policy "anon can update activities"
  on public.activities
  for update
  to anon
  using (true)
  with check (true);

create policy "anon can delete activities"
  on public.activities
  for delete
  to anon
  using (true);

-- Seed data: the two activities that were previously hardcoded in the
-- Flutter app (lib/features/activities/hardcoded_activities.dart), so the
-- app's behavior is unchanged the moment it switches to Supabase. The
-- basketball prompt below includes the richer introduction requested after
-- physical-device testing (was previously too short).
insert into public.activities
  (title, type, prompt, participants, interaction_mode, enabled, sort_order)
values
  (
    'Baschet',
    'lesson',
    $prompt$Activitate: Lecție de baschet.

Înainte de a adresa prima întrebare Mariei, oferă o introducere puțin mai bogată despre baschet, de aproximativ 60-90 de secunde, formată din 6-8 idei scurte, cu propoziții simple, potrivite pentru copii de 8-10 ani, cu un ton cald, energic și prietenos. Nu transforma introducerea într-o prelegere lungă.

În introducere, atinge pe scurt fiecare dintre aceste idei:
- baschetul este un sport de echipă;
- coșul este locul în care trebuie aruncată mingea pentru a marca puncte;
- pasa înseamnă a trimite mingea către un coechipier;
- driblingul înseamnă a lovi ritmic mingea de podea atunci când te deplasezi cu ea;
- aruncarea la coș cere precizie și exercițiu;
- lucrul în echipă este esențial pentru a câștiga;
- regulile există ca jocul să fie corect și sigur pentru toți;
- antrenamentul și coordonarea te ajută să devii tot mai bun.

Abia după această introducere, adresează-te copiilor participanți unul câte unul, STRICT în ordinea indicată mai jos (vezi lista de participanți).

Reguli obligatorii pentru întrebări:
- Pune o singură întrebare unui singur copil, legată de introducerea de mai sus (de exemplu despre pasă, dribling, coș, echipă, precizie sau reguli), apoi încheie răspunsul tău și așteaptă răspunsul lui. Nu pune mai multe întrebări în același răspuns.
- Nu trece la următorul copil înainte de a primi răspunsul copilului curent.
- Nu inventa alți participanți în afara celor indicați în lista de participanți.
- După ce copilul răspunde, reacționează scurt și pozitiv, oferă o mică lămurire dacă ajută, apoi treci la următorul copil din ordine.
- După ce toți copiii din listă au răspuns, oferă o concluzie foarte scurtă despre baschet și încheie lecția în mod natural.$prompt$,
    array['Maria', 'Sofia', 'Alex', 'Ștefan'],
    'push_to_talk',
    true,
    0
  ),
  (
    'Conversație liberă',
    'conversation',
    $prompt$Activitate: Conversație liberă.

Lasă copilul să vorbească liber despre orice temă îl interesează, de exemplu: animale, știință, spațiu, tehnologie, școală, sport, cultură generală sau lucruri din viața de zi cu zi potrivite vârstei lui.

Răspunde de obicei în una până la trei propoziții scurte. Fii prietenos și curios, dar nu domina conversația — lasă copilul să vorbească mai mult decât tine.$prompt$,
    '{}',
    'free_conversation',
    true,
    1
  );
