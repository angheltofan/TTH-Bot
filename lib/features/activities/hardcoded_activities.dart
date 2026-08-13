import 'activity.dart';

/// Basketball lesson: TTH Bot gives a short explanation, then addresses each
/// participant one at a time (see [Activity.participants] for the exact
/// order) with a single question per turn. The turn-taking itself is not
/// tracked in Dart — it's driven entirely by this prompt plus the fact that
/// the whole activity runs over one continuous Live API session, so Gemini
/// keeps the conversation history across every push-to-talk turn.
///
/// Kept as a read-only fallback/testing activity now that Supabase is the
/// normal runtime source (see [SupabaseActivityRepository]) — this text
/// must stay identical to the `Baschet` seed row in
/// supabase/migrations/20260812124951_create_activities.sql.
const Activity basketballActivity = Activity(
  id: 'baschet',
  title: 'Baschet',
  type: ActivityType.lesson,
  interactionMode: InteractionMode.pushToTalk,
  participants: ['Maria', 'Sofia', 'Alex', 'Ștefan'],
  systemPrompt: _basketballSystemPrompt,
);

const String _basketballSystemPrompt = '''
Activitate: Lecție de baschet.

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
- După ce toți copiii din listă au răspuns, oferă o concluzie foarte scurtă despre baschet și încheie lecția în mod natural.
''';

/// Free conversation: no fixed participants, no fixed topic — the child
/// leads, hands-free. Genuinely [InteractionMode.freeConversation] as of
/// the hands-free milestone — automatic server-side VAD, continuous
/// microphone, no press required. See VoiceSessionController.
const Activity freeConversationActivity = Activity(
  id: 'conversatie-libera',
  title: 'Conversație liberă',
  type: ActivityType.conversation,
  interactionMode: InteractionMode.freeConversation,
  systemPrompt: _freeConversationSystemPrompt,
  sortOrder: 1,
);

const String _freeConversationSystemPrompt = '''
Activitate: Conversație liberă.

Lasă copilul să vorbească liber despre orice temă îl interesează, de exemplu: animale, știință, spațiu, tehnologie, școală, sport, cultură generală sau lucruri din viața de zi cu zi potrivite vârstei lui.

Răspunde de obicei în una până la trei propoziții scurte. Fii prietenos și curios, dar nu domina conversația — lasă copilul să vorbească mai mult decât tine.
''';
