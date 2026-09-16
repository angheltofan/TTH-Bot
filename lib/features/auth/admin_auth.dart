import 'dart:async';

import 'package:supabase_flutter/supabase_flutter.dart';

/// The only username the web editor login accepts.
const String adminUsername = 'admin';

/// The Supabase Auth email of the single administrator account. Supplied
/// at build time (`--dart-define=TTH_ADMIN_AUTH_EMAIL=...`, see
/// scripts/build_web.ps1) so it is not kept in source. It is never shown in
/// the UI and never logged; note it is still present in the compiled web
/// bundle — security rests on the password and the database allow-list.
const String configuredAdminAuthEmail = String.fromEnvironment(
  'TTH_ADMIN_AUTH_EMAIL',
);

/// The auth email for a typed [username], or `null` when the username is
/// not [adminUsername] or no admin email is configured. Surrounding
/// whitespace (e.g. from a phone keyboard) is ignored; nothing else is.
String? adminEmailForUsername(String username, {required String adminEmail}) {
  if (username.trim() != adminUsername) return null;
  if (!isPlausibleAdminEmail(adminEmail)) return null;
  return adminEmail;
}

bool isPlausibleAdminEmail(String email) =>
    RegExp(r'^[^@\s]+@[^@\s]+\.[^@\s]+$').hasMatch(email);

enum AdminSessionStatus {
  /// No session at all.
  signedOut,

  /// A session whose access token is still valid.
  active,

  /// A stored session whose access token expired (a refresh may be running).
  expired,
}

enum AdminAuthChange {
  signedIn,

  /// Signed out on purpose (logout here or in another tab).
  signedOut,

  /// The session was lost involuntarily (refresh token rejected, missing).
  sessionExpired,
}

/// What the web editor needs from authentication. [SupabaseAdminAuth] is
/// the real implementation; widget tests use a fake.
///
/// Implementations must never log or expose the password, the internal
/// email, sessions, tokens or server error text.
abstract class AdminAuth {
  /// Whether an admin auth email was configured for this build.
  bool get isConfigured;

  AdminSessionStatus get sessionStatus;

  Stream<AdminAuthChange> get changes;

  /// Signs in with the visible [username] and the typed [password]. Returns
  /// `true` only when a session was created; every failure — wrong
  /// username, wrong password, network, server — is the same `false`.
  Future<bool> signIn({required String username, required String password});

  /// Ends the session. Never throws.
  Future<void> signOut();
}

class SupabaseAdminAuth implements AdminAuth {
  SupabaseAdminAuth(this._auth, {String adminEmail = configuredAdminAuthEmail})
    : _adminEmail = adminEmail;

  final GoTrueClient _auth;
  final String _adminEmail;

  @override
  bool get isConfigured => isPlausibleAdminEmail(_adminEmail);

  @override
  AdminSessionStatus get sessionStatus {
    final session = _auth.currentSession;
    if (session == null) return AdminSessionStatus.signedOut;
    return session.isExpired
        ? AdminSessionStatus.expired
        : AdminSessionStatus.active;
  }

  @override
  Stream<AdminAuthChange> get changes => _auth.onAuthStateChange.transform(
    StreamTransformer<AuthState, AdminAuthChange>.fromHandlers(
      handleData: (state, sink) {
        final change = _toChange(state);
        if (change != null) sink.add(change);
      },
      // Stream errors are never forwarded or logged (they carry server
      // text). Only their effect matters: a session that is gone.
      handleError: (error, stackTrace, sink) {
        if (_auth.currentSession == null) {
          sink.add(AdminAuthChange.sessionExpired);
        }
      },
    ),
  );

  AdminAuthChange? _toChange(AuthState state) {
    switch (state.event) {
      case AuthChangeEvent.initialSession:
        final session = state.session;
        if (session == null) return AdminAuthChange.signedOut;
        // An expired stored session is being refreshed; wait for the
        // tokenRefreshed / signedOut that follows.
        return session.isExpired ? null : AdminAuthChange.signedIn;
      case AuthChangeEvent.signedIn:
      case AuthChangeEvent.tokenRefreshed:
      case AuthChangeEvent.userUpdated:
      case AuthChangeEvent.mfaChallengeVerified:
        return state.session == null ? null : AdminAuthChange.signedIn;
      case AuthChangeEvent.signedOut:
        return switch (state.signOutReason) {
          SignOutReason.sessionExpired ||
          SignOutReason.sessionMissing => AdminAuthChange.sessionExpired,
          _ => AdminAuthChange.signedOut,
        };
      default:
        return null;
    }
  }

  @override
  Future<bool> signIn({
    required String username,
    required String password,
  }) async {
    final email = adminEmailForUsername(username, adminEmail: _adminEmail);
    // A wrong username never reaches the network.
    if (email == null || password.isEmpty) return false;
    try {
      final response = await _auth.signInWithPassword(
        email: email,
        password: password,
      );
      return response.session != null;
    } catch (_) {
      // Deliberately silent: the error may contain the email or server text.
      return false;
    }
  }

  @override
  Future<void> signOut() async {
    try {
      // Removes the local session first, then revokes it on the server.
      await _auth.signOut();
    } catch (_) {
      // The local session is already gone; nothing useful to report.
    }
  }
}
