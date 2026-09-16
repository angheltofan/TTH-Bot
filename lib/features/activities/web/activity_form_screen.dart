import 'package:flutter/material.dart';

import '../activity.dart';
import '../activity_failure.dart';
import '../activity_repository.dart';
import '../participants.dart';

/// Create/edit form for one activity. Used for both "Activitate nouă" and
/// the per-row "Edit" action — only [existing] differs between the two.
///
/// Pops `true` when a save succeeds (so [ActivityWebApp] knows to reload
/// the list) and `false`/`null` on cancel.
class ActivityFormScreen extends StatefulWidget {
  const ActivityFormScreen({
    super.key,
    required this.repository,
    this.existing,
    this.onSessionExpired,
  });

  final ActivityRepository repository;
  final Activity? existing;

  /// Called when saving fails because the session is no longer valid.
  final VoidCallback? onSessionExpired;

  @override
  State<ActivityFormScreen> createState() => _ActivityFormScreenState();
}

class _ActivityFormScreenState extends State<ActivityFormScreen> {
  final _formKey = GlobalKey<FormState>();
  late final TextEditingController _titleController;
  late final TextEditingController _promptController;
  late final TextEditingController _participantsController;
  late final TextEditingController _sortOrderController;
  late ActivityType _type;
  late InteractionMode _interactionMode;
  late bool _enabled;

  bool _saving = false;
  String? _errorMessage;

  bool get _isEditing => widget.existing != null;

  @override
  void initState() {
    super.initState();
    final existing = widget.existing;
    _titleController = TextEditingController(text: existing?.title ?? '');
    _promptController = TextEditingController(
      text: existing?.systemPrompt ?? '',
    );
    _participantsController = TextEditingController(
      text: formatParticipants(existing?.participants ?? const []),
    );
    _sortOrderController = TextEditingController(
      text: (existing?.sortOrder ?? 0).toString(),
    );
    _type = existing?.type ?? ActivityType.lesson;
    _interactionMode = existing?.interactionMode ?? InteractionMode.pushToTalk;
    _enabled = existing?.enabled ?? true;
  }

  @override
  void dispose() {
    _titleController.dispose();
    _promptController.dispose();
    _participantsController.dispose();
    _sortOrderController.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    if (!_formKey.currentState!.validate()) return;

    setState(() {
      _saving = true;
      _errorMessage = null;
    });

    final activity = Activity(
      id: widget.existing?.id ?? '',
      title: _titleController.text.trim(),
      type: _type,
      systemPrompt: _promptController.text.trim(),
      interactionMode: _interactionMode,
      participants: parseParticipants(_participantsController.text),
      enabled: _enabled,
      sortOrder: int.parse(_sortOrderController.text.trim()),
    );

    try {
      if (_isEditing) {
        await widget.repository.updateActivity(activity);
      } else {
        await widget.repository.createActivity(activity);
      }
      if (!mounted) return;
      Navigator.of(context).pop(true);
    } catch (e) {
      if (!mounted) return;
      final failure = activityFailureOf(e);
      setState(() {
        _errorMessage = activityFailureMessage(failure, ActivityAction.save);
        _saving = false;
      });
      if (failure == ActivityFailure.sessionExpired) {
        widget.onSessionExpired?.call();
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(_isEditing ? 'Editează activitate' : 'Activitate nouă'),
      ),
      // Scrollable spans the full viewport width/height (see
      // ActivityWebApp's build() for the same fix and why) so the mouse
      // wheel scrolls the page from anywhere, not just over the centered
      // form column.
      body: SingleChildScrollView(
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 760),
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Form(
                key: _formKey,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    TextFormField(
                      controller: _titleController,
                      decoration: const InputDecoration(labelText: 'Titlu'),
                      validator: (value) =>
                          (value == null || value.trim().isEmpty)
                          ? 'Titlul este obligatoriu'
                          : null,
                    ),
                    const SizedBox(height: 16),
                    DropdownButtonFormField<ActivityType>(
                      initialValue: _type,
                      isExpanded: true,
                      decoration: const InputDecoration(labelText: 'Tip'),
                      items: const [
                        DropdownMenuItem(
                          value: ActivityType.lesson,
                          child: Text('Lecție'),
                        ),
                        DropdownMenuItem(
                          value: ActivityType.game,
                          child: Text('Joc'),
                        ),
                        DropdownMenuItem(
                          value: ActivityType.conversation,
                          child: Text('Conversație'),
                        ),
                      ],
                      onChanged: (value) =>
                          setState(() => _type = value ?? _type),
                    ),
                    const SizedBox(height: 16),
                    TextFormField(
                      controller: _promptController,
                      decoration: const InputDecoration(
                        labelText: 'Prompt',
                        alignLabelWithHint: true,
                      ),
                      minLines: 8,
                      maxLines: 20,
                      validator: (value) =>
                          (value == null || value.trim().isEmpty)
                          ? 'Promptul este obligatoriu'
                          : null,
                    ),
                    const SizedBox(height: 16),
                    TextFormField(
                      controller: _participantsController,
                      decoration: const InputDecoration(
                        labelText: 'Copiii participanți',
                        helperText:
                            'Separă prin virgulă sau câte un nume pe linie. '
                            'Ordinea contează — TTH Bot îi adresează în această '
                            'ordine.',
                        helperMaxLines: 2,
                      ),
                      minLines: 2,
                      maxLines: 6,
                    ),
                    const SizedBox(height: 16),
                    DropdownButtonFormField<InteractionMode>(
                      initialValue: _interactionMode,
                      isExpanded: true,
                      decoration: const InputDecoration(
                        labelText: 'Mod conversație',
                      ),
                      items: const [
                        DropdownMenuItem(
                          value: InteractionMode.pushToTalk,
                          child: Text('Push-to-talk'),
                        ),
                        DropdownMenuItem(
                          value: InteractionMode.freeConversation,
                          child: Text('Conversație liberă'),
                        ),
                      ],
                      onChanged: (value) => setState(
                        () => _interactionMode = value ?? _interactionMode,
                      ),
                    ),
                    const SizedBox(height: 16),
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.center,
                      children: [
                        Expanded(
                          child: TextFormField(
                            controller: _sortOrderController,
                            decoration: const InputDecoration(
                              labelText: 'Ordine',
                            ),
                            keyboardType: TextInputType.number,
                            validator: (value) =>
                                int.tryParse((value ?? '').trim()) == null
                                ? 'Introdu un număr întreg'
                                : null,
                          ),
                        ),
                        const SizedBox(width: 24),
                        Expanded(
                          child: SwitchListTile(
                            contentPadding: EdgeInsets.zero,
                            title: const Text('Activ'),
                            value: _enabled,
                            onChanged: (value) =>
                                setState(() => _enabled = value),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 24),
                    if (_errorMessage != null)
                      Padding(
                        padding: const EdgeInsets.only(bottom: 16),
                        child: Text(
                          _errorMessage!,
                          style: const TextStyle(color: Colors.redAccent),
                        ),
                      ),
                    // Wrap, not Row: the buttons stack on a narrow phone
                    // instead of overflowing.
                    Wrap(
                      alignment: WrapAlignment.center,
                      spacing: 12,
                      runSpacing: 8,
                      children: [
                        OutlinedButton(
                          onPressed: _saving
                              ? null
                              : () => Navigator.of(context).pop(false),
                          child: const Text('Anulează'),
                        ),
                        FilledButton(
                          onPressed: _saving ? null : _save,
                          child: _saving
                              ? const SizedBox(
                                  width: 18,
                                  height: 18,
                                  child: CircularProgressIndicator(
                                    strokeWidth: 2,
                                  ),
                                )
                              : const Text('Salvează'),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
