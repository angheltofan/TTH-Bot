import 'dart:async';

import 'package:tth_bot/features/auth/admin_auth.dart';

/// Test-only password. Deliberately NOT the administrator's real password,
/// which never appears in the repository.
const String fakeAdminPassword = 'test-only-password';

/// In-memory [AdminAuth] for widget tests.
class FakeAdminAuth implements AdminAuth {
  FakeAdminAuth({
    this.status = AdminSessionStatus.signedOut,
    this.isConfigured = true,
  });

  AdminSessionStatus status;

  @override
  bool isConfigured;

  final StreamController<AdminAuthChange> _changes =
      StreamController<AdminAuthChange>.broadcast();

  final List<String> attemptedUsernames = [];
  final List<String> attemptedPasswords = [];
  int signOutCalls = 0;

  /// When set, sign-in waits for it (to observe the busy state).
  Completer<void>? holdSignIn;

  @override
  AdminSessionStatus get sessionStatus => status;

  @override
  Stream<AdminAuthChange> get changes => _changes.stream;

  void emit(AdminAuthChange change) => _changes.add(change);

  @override
  Future<bool> signIn({
    required String username,
    required String password,
  }) async {
    attemptedUsernames.add(username);
    attemptedPasswords.add(password);
    await holdSignIn?.future;
    final ok =
        username.trim() == adminUsername && password == fakeAdminPassword;
    if (ok) {
      status = AdminSessionStatus.active;
      _changes.add(AdminAuthChange.signedIn);
    }
    return ok;
  }

  @override
  Future<void> signOut() async {
    signOutCalls++;
    status = AdminSessionStatus.signedOut;
    _changes.add(AdminAuthChange.signedOut);
  }
}
