import 'package:flutter/material.dart';

import '../../core/theme/tth_colors.dart';

/// The same message for every failed login, whatever the cause.
const String adminLoginFailedMessage =
    'Autentificare eșuată. Verifică datele și încearcă din nou.';

/// Login page of the web editor: username + password only. No sign-up, no
/// password reset, no guest access. The password field is cleared after
/// every attempt, successful or not.
class AdminLoginScreen extends StatefulWidget {
  const AdminLoginScreen({super.key, required this.onSubmit, this.notice});

  /// Returns `true` when the login succeeded.
  final Future<bool> Function(String username, String password) onSubmit;

  /// Informational banner, e.g. "the session expired".
  final String? notice;

  @override
  State<AdminLoginScreen> createState() => _AdminLoginScreenState();
}

class _AdminLoginScreenState extends State<AdminLoginScreen> {
  final _usernameController = TextEditingController();
  final _passwordController = TextEditingController();
  bool _busy = false;
  bool _failed = false;
  bool _noticeDismissed = false;

  @override
  void dispose() {
    _usernameController.dispose();
    _passwordController.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    if (_busy) return;
    final username = _usernameController.text;
    final password = _passwordController.text;
    _passwordController.clear();
    setState(() {
      _busy = true;
      _failed = false;
      _noticeDismissed = true;
    });

    var succeeded = false;
    if (username.trim().isNotEmpty && password.isNotEmpty) {
      try {
        succeeded = await widget.onSubmit(username, password);
      } catch (_) {
        succeeded = false;
      }
    }

    if (!mounted) return;
    setState(() {
      _busy = false;
      _failed = !succeeded;
    });
  }

  @override
  Widget build(BuildContext context) {
    final notice = _noticeDismissed ? null : widget.notice;
    return Scaffold(
      body: SafeArea(
        child: SingleChildScrollView(
          child: Center(
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 420),
              child: Padding(
                padding: const EdgeInsets.symmetric(
                  horizontal: 24,
                  vertical: 32,
                ),
                child: AutofillGroup(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Image.asset(
                        'assets/branding/tales_tech_hub_logo.png',
                        height: 100,
                        errorBuilder: (context, error, stackTrace) =>
                            const Text(
                              'Tales & Tech Hub',
                              textAlign: TextAlign.center,
                              style: TextStyle(
                                color: TthColors.brandBlue,
                                fontSize: 20,
                                fontWeight: FontWeight.w700,
                              ),
                            ),
                      ),
                      const SizedBox(height: 32),
                      const Text(
                        'TTH Bot – Activități',
                        textAlign: TextAlign.center,
                        style: TextStyle(
                          color: TthColors.webTextDark,
                          fontSize: 24,
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                      const SizedBox(height: 8),
                      const Text(
                        'Autentificare administrator',
                        textAlign: TextAlign.center,
                        style: TextStyle(
                          color: TthColors.webTextMuted,
                          fontSize: 15,
                        ),
                      ),
                      const SizedBox(height: 28),
                      if (notice != null) ...[
                        _Banner(message: notice, isError: false),
                        const SizedBox(height: 16),
                      ],
                      Card(
                        child: Padding(
                          padding: const EdgeInsets.all(24),
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: [
                              TextField(
                                controller: _usernameController,
                                enabled: !_busy,
                                autocorrect: false,
                                enableSuggestions: false,
                                textInputAction: TextInputAction.next,
                                autofillHints: const [AutofillHints.username],
                                decoration: const InputDecoration(
                                  labelText: 'Utilizator',
                                  prefixIcon: Icon(Icons.person_outline),
                                ),
                              ),
                              const SizedBox(height: 16),
                              TextField(
                                controller: _passwordController,
                                enabled: !_busy,
                                obscureText: true,
                                autocorrect: false,
                                enableSuggestions: false,
                                textInputAction: TextInputAction.done,
                                autofillHints: const [AutofillHints.password],
                                onSubmitted: (_) => _submit(),
                                decoration: const InputDecoration(
                                  labelText: 'Parolă',
                                  prefixIcon: Icon(Icons.lock_outline),
                                ),
                              ),
                              if (_failed) ...[
                                const SizedBox(height: 16),
                                const _Banner(
                                  message: adminLoginFailedMessage,
                                  isError: true,
                                ),
                              ],
                              const SizedBox(height: 24),
                              FilledButton(
                                onPressed: _busy ? null : _submit,
                                child: _busy
                                    ? const SizedBox(
                                        width: 18,
                                        height: 18,
                                        child: CircularProgressIndicator(
                                          strokeWidth: 2,
                                        ),
                                      )
                                    : const Text('Autentificare'),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _Banner extends StatelessWidget {
  const _Banner({required this.message, required this.isError});

  final String message;
  final bool isError;

  @override
  Widget build(BuildContext context) {
    final color = isError ? Colors.red.shade700 : TthColors.brandBlue;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(12),
      ),
      child: Row(
        children: [
          Icon(
            isError ? Icons.error_outline : Icons.info_outline,
            color: color,
            size: 20,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(message, style: TextStyle(color: color, fontSize: 14)),
          ),
        ],
      ),
    );
  }
}
