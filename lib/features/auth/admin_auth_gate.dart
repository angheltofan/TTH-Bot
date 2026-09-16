import 'dart:async';

import 'package:flutter/material.dart';

import '../../core/theme/tth_colors.dart';
import 'admin_auth.dart';
import 'admin_login_screen.dart';

const String sessionExpiredNotice =
    'Sesiunea a expirat. Autentifică-te din nou.';

/// Callbacks the editor uses to leave the authenticated area.
class AdminSessionControls {
  const AdminSessionControls({
    required this.logout,
    required this.sessionExpired,
  });

  /// The administrator pressed "Deconectare".
  final VoidCallback logout;

  /// The server rejected the session (e.g. an expired token on a write).
  final VoidCallback sessionExpired;
}

enum _GateView { checking, login, editor, unconfigured }

/// Shows only [AdminLoginScreen] until there is a valid session, then the
/// editor built by [editorBuilder]. Restores an existing session on start,
/// and returns to the login page (closing any open form or dialog) on
/// logout or when the session expires.
///
/// This is a convenience for the user, not the security boundary: the
/// database allows activity writes only for the allow-listed
/// administrator, whatever the browser shows.
class AdminAuthGate extends StatefulWidget {
  const AdminAuthGate({
    super.key,
    required this.auth,
    required this.editorBuilder,
    this.sessionCheckInterval = const Duration(seconds: 20),
  });

  final AdminAuth auth;
  final Widget Function(BuildContext context, AdminSessionControls controls)
  editorBuilder;

  /// How often the gate re-checks the session while the editor is open. A
  /// session that stays expired for two consecutive checks (the automatic
  /// refresh had its chance) sends the user back to the login page.
  final Duration sessionCheckInterval;

  @override
  State<AdminAuthGate> createState() => _AdminAuthGateState();
}

class _AdminAuthGateState extends State<AdminAuthGate> {
  late _GateView _view;
  String? _notice;
  int _editorGeneration = 0;
  int _expiredChecks = 0;
  StreamSubscription<AdminAuthChange>? _subscription;
  Timer? _timer;

  late final AdminSessionControls _controls = AdminSessionControls(
    logout: _logout,
    sessionExpired: _expire,
  );

  @override
  void initState() {
    super.initState();
    if (!widget.auth.isConfigured) {
      _view = _GateView.unconfigured;
      return;
    }
    _view = switch (widget.auth.sessionStatus) {
      AdminSessionStatus.active => _GateView.editor,
      AdminSessionStatus.expired => _GateView.checking,
      AdminSessionStatus.signedOut => _GateView.login,
    };
    _subscription = widget.auth.changes.listen(_onChange);
    _timer = Timer.periodic(widget.sessionCheckInterval, (_) => _check());
  }

  @override
  void dispose() {
    _subscription?.cancel();
    _timer?.cancel();
    super.dispose();
  }

  void _onChange(AdminAuthChange change) {
    if (!mounted) return;
    switch (change) {
      case AdminAuthChange.signedIn:
        if (widget.auth.sessionStatus == AdminSessionStatus.active) {
          _showEditor();
        }
      case AdminAuthChange.signedOut:
        if (_view != _GateView.login) _showLogin(notice: null);
      case AdminAuthChange.sessionExpired:
        if (_view != _GateView.login) _showLogin(notice: sessionExpiredNotice);
    }
  }

  void _check() {
    if (!mounted || _view == _GateView.login) return;
    switch (widget.auth.sessionStatus) {
      case AdminSessionStatus.active:
        _expiredChecks = 0;
        if (_view == _GateView.checking) _showEditor();
      case AdminSessionStatus.signedOut:
        _expire();
      case AdminSessionStatus.expired:
        _expiredChecks++;
        if (_expiredChecks >= 2) _expire();
    }
  }

  Future<bool> _signIn(String username, String password) async {
    final ok = await widget.auth.signIn(username: username, password: password);
    if (ok && mounted) _showEditor();
    return ok;
  }

  void _logout() {
    _showLogin(notice: null);
    widget.auth.signOut();
  }

  void _expire() {
    if (_view == _GateView.login) return;
    _showLogin(notice: sessionExpiredNotice);
    widget.auth.signOut();
  }

  void _showEditor() {
    if (_view == _GateView.editor) return;
    setState(() {
      _view = _GateView.editor;
      _notice = null;
      _expiredChecks = 0;
      _editorGeneration++;
    });
  }

  void _showLogin({required String? notice}) {
    // Close forms and dialogs opened from the editor, so nothing of the
    // authenticated area stays on screen.
    Navigator.maybeOf(context)?.popUntil((route) => route.isFirst);
    setState(() {
      _view = _GateView.login;
      _notice = notice;
      _expiredChecks = 0;
    });
  }

  @override
  Widget build(BuildContext context) {
    switch (_view) {
      case _GateView.unconfigured:
        return const AdminUnavailableScreen(
          message:
              'Autentificarea nu este configurată pentru această versiune.',
        );
      case _GateView.checking:
        return const Scaffold(body: Center(child: CircularProgressIndicator()));
      case _GateView.login:
        return AdminLoginScreen(
          key: ValueKey('login-$_editorGeneration-$_notice'),
          onSubmit: _signIn,
          notice: _notice,
        );
      case _GateView.editor:
        return KeyedSubtree(
          key: ValueKey('editor-$_editorGeneration'),
          child: widget.editorBuilder(context, _controls),
        );
    }
  }
}

/// Shown instead of the login page when the web editor cannot work at all
/// (no Supabase connection, or no admin account configured in the build).
class AdminUnavailableScreen extends StatelessWidget {
  const AdminUnavailableScreen({super.key, required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(
                Icons.cloud_off,
                size: 40,
                color: TthColors.webTextMuted,
              ),
              const SizedBox(height: 12),
              Text(
                message,
                textAlign: TextAlign.center,
                style: const TextStyle(color: TthColors.webTextDark),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
