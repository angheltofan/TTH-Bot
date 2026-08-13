import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:supabase_flutter/supabase_flutter.dart';

import 'core/supabase/supabase_config.dart';
import 'core/theme/tth_web_theme.dart';
import 'features/activities/activity_repository.dart';
import 'features/activities/supabase_activity_repository.dart';
import 'features/activities/web/activity_web_app.dart';
import 'features/robot/robot_home_screen.dart';
import 'features/voice/gemini_auth.dart';
import 'features/voice/gemini_token_service.dart';

/// DEV-ONLY: supplied at build/run time via
/// `--dart-define=GEMINI_API_KEY=...`. Never hardcode a real key here and
/// never commit one. This is now only the *fallback* auth path — see
/// [resolveGeminiAuth] — used when the `gemini-token` Supabase Edge
/// Function isn't reachable (not deployed yet, no network, ...). The
/// preferred path is an ephemeral token minted by that function, which
/// never exposes a real Gemini API key to the app at all.
const String _geminiApiKey = String.fromEnvironment('GEMINI_API_KEY');

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  ActivityRepository activityRepository = const HardcodedActivityRepository();
  SupabaseClient? supabaseClient;
  try {
    await Supabase.initialize(
      url: SupabaseConfig.url,
      publishableKey: SupabaseConfig.publishableKey,
    );
    supabaseClient = Supabase.instance.client;
    activityRepository = SupabaseActivityRepository(supabaseClient);
    debugPrint('[Supabase] initialized; using SupabaseActivityRepository');
  } catch (e, stackTrace) {
    debugPrint(
      '[Supabase] initialize failed, falling back to hardcoded activities: '
      '$e\n$stackTrace',
    );
  }

  if (!kIsWeb) {
    await SystemChrome.setPreferredOrientations([
      DeviceOrientation.landscapeLeft,
      DeviceOrientation.landscapeRight,
    ]);
    // Edge-to-edge robot face, per Phase 5 Part C — the phone screen is the
    // robot's face, embedded in a physical shell, so the Android system UI
    // (nav/status bars) should stay out of the way.
    await SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
  }

  runApp(
    TthBotApp(
      activityRepository: activityRepository,
      supabaseClient: supabaseClient,
    ),
  );
}

class TthBotApp extends StatelessWidget {
  const TthBotApp({
    super.key,
    required this.activityRepository,
    required this.supabaseClient,
  });

  final ActivityRepository activityRepository;
  final SupabaseClient? supabaseClient;

  Future<GeminiAuth> _resolveAuth() {
    final client = supabaseClient;
    return resolveGeminiAuth(
      fetchEphemeralToken: client == null
          ? null
          : GeminiTokenService(client).fetchEphemeralToken,
      devApiKey: _geminiApiKey,
    );
  }

  @override
  Widget build(BuildContext context) {
    if (kIsWeb) {
      return MaterialApp(
        title: 'TTH Bot – Activități',
        debugShowCheckedModeBanner: false,
        theme: buildTthWebTheme(),
        home: ActivityWebApp(repository: activityRepository),
      );
    }

    return MaterialApp(
      title: 'TTH Bot',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true),
      home: RobotHomeScreen(
        repository: activityRepository,
        resolveAuth: _resolveAuth,
      ),
    );
  }
}
