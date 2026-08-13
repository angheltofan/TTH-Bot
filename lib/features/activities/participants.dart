/// Parses the web form's free-form "Copiii participanți" field into a
/// clean, ordered participant list.
///
/// Accepts comma-separated ("Maria, Sofia, Alex") and/or newline-separated
/// input (or a mix of both). Entries are trimmed and empty entries are
/// dropped. Order is preserved as typed — nothing is deduplicated or
/// alphabetized, since order is the exact sequence TTH Bot addresses
/// children in.
List<String> parseParticipants(String raw) {
  return raw
      .split(RegExp(r'[,\n]'))
      .map((entry) => entry.trim())
      .where((entry) => entry.isNotEmpty)
      .toList();
}

/// Inverse of [parseParticipants], for pre-filling the form field when
/// editing an existing activity.
String formatParticipants(List<String> participants) => participants.join(', ');
