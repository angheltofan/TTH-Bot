import 'package:flutter/foundation.dart';

/// What kind of activity this is. Maps to the `type` check constraint in
/// the `activities` table (see supabase/migrations).
enum ActivityType { lesson, conversation, game }

extension ActivityTypeDb on ActivityType {
  String get dbValue => switch (this) {
    ActivityType.lesson => 'lesson',
    ActivityType.conversation => 'conversation',
    ActivityType.game => 'game',
  };

  static ActivityType fromDbValue(String value) => switch (value) {
    'lesson' => ActivityType.lesson,
    'conversation' => ActivityType.conversation,
    'game' => ActivityType.game,
    _ => throw FormatException('Unknown activity type from database: $value'),
  };
}

/// How the child interacts with TTH Bot during this activity. Maps to the
/// `interaction_mode` check constraint in the `activities` table.
enum InteractionMode {
  /// Press-and-hold to talk, release to let Gemini respond.
  pushToTalk,

  /// Hands-free, continuous listening driven by server-side VAD — no press
  /// required. See `VoiceSessionController.startFreeConversation`.
  freeConversation,
}

extension InteractionModeDb on InteractionMode {
  String get dbValue => switch (this) {
    InteractionMode.pushToTalk => 'push_to_talk',
    InteractionMode.freeConversation => 'free_conversation',
  };

  static InteractionMode fromDbValue(String value) => switch (value) {
    'push_to_talk' => InteractionMode.pushToTalk,
    'free_conversation' => InteractionMode.freeConversation,
    _ => throw FormatException(
      'Unknown interaction mode from database: $value',
    ),
  };
}

/// User-facing Romanian labels, shared by the Android settings modal and
/// the Web activity list/form so both surfaces always describe the same
/// activity the same way — previously each screen defined its own copy of
/// these switches, and they had quietly drifted apart for
/// [InteractionMode.freeConversation] ("Hands-free" on Android vs.
/// "Conversație liberă" on Web, the latter matching the activity's own
/// Romanian title in seed data).
extension ActivityTypeLabel on ActivityType {
  String get label => switch (this) {
    ActivityType.lesson => 'Lecție',
    ActivityType.game => 'Joc',
    ActivityType.conversation => 'Conversație',
  };
}

extension InteractionModeLabel on InteractionMode {
  String get label => switch (this) {
    InteractionMode.pushToTalk => 'Push-to-talk',
    InteractionMode.freeConversation => 'Conversație liberă',
  };
}

/// A single TTH Bot activity — today either hardcoded Dart
/// ([HardcodedActivityRepository]) or a row from the `activities` Supabase
/// table ([SupabaseActivityRepository]/[Activity.fromMap]).
///
/// Deliberately plain data with no behavior, so neither the UI nor the
/// prompt composer needs to know or care where it came from.
@immutable
class Activity {
  const Activity({
    required this.id,
    required this.title,
    required this.type,
    required this.systemPrompt,
    required this.interactionMode,
    this.participants = const [],
    this.enabled = true,
    this.sortOrder = 0,
    this.createdAt,
    this.updatedAt,
  });

  /// Row UUID once persisted in Supabase; an arbitrary stable slug for the
  /// still-hardcoded activities.
  final String id;

  final String title;
  final ActivityType type;

  /// Activity-specific system prompt fragment only — general TTH Bot
  /// behavior lives in [eduBotBasePrompt] instead. Combine the two (and any
  /// participant context) via `composeSystemInstruction` rather than
  /// sending this alone to Gemini.
  final String systemPrompt;

  final InteractionMode interactionMode;

  /// Named children taking part, in the exact order TTH Bot should address
  /// them. Empty for activities with no fixed participants (e.g. free
  /// conversation).
  final List<String> participants;

  /// Whether this activity should be offered on the Android selector.
  /// Disabled activities still show (and stay editable) in the web
  /// configuration list.
  final bool enabled;

  /// Manual ordering for both the Android selector and the web list;
  /// ascending, ties broken by title.
  final int sortOrder;

  final DateTime? createdAt;
  final DateTime? updatedAt;

  /// Parses one `activities` row as returned by Supabase.
  factory Activity.fromMap(Map<String, dynamic> map) {
    return Activity(
      id: map['id'] as String,
      title: map['title'] as String,
      type: ActivityTypeDb.fromDbValue(map['type'] as String),
      systemPrompt: map['prompt'] as String,
      interactionMode: InteractionModeDb.fromDbValue(
        map['interaction_mode'] as String,
      ),
      participants: (map['participants'] as List<dynamic>? ?? const [])
          .map((p) => p as String)
          .toList(),
      enabled: map['enabled'] as bool? ?? true,
      sortOrder: map['sort_order'] as int? ?? 0,
      createdAt: map['created_at'] == null
          ? null
          : DateTime.parse(map['created_at'] as String),
      updatedAt: map['updated_at'] == null
          ? null
          : DateTime.parse(map['updated_at'] as String),
    );
  }

  /// Full round-trip map, including server-owned fields. Mainly useful for
  /// debugging/equality — writes should go through [toInsertMap] or
  /// [toUpdateMap] instead, which correctly omit server-owned columns.
  Map<String, dynamic> toMap() => {
    'id': id,
    'title': title,
    'type': type.dbValue,
    'prompt': systemPrompt,
    'participants': participants,
    'interaction_mode': interactionMode.dbValue,
    'enabled': enabled,
    'sort_order': sortOrder,
    'created_at': createdAt?.toIso8601String(),
    'updated_at': updatedAt?.toIso8601String(),
  };

  /// Payload for `insert()`: omits `id` (DB-generated) and the timestamps
  /// (DB-defaulted / trigger-maintained).
  Map<String, dynamic> toInsertMap() => {
    'title': title,
    'type': type.dbValue,
    'prompt': systemPrompt,
    'participants': participants,
    'interaction_mode': interactionMode.dbValue,
    'enabled': enabled,
    'sort_order': sortOrder,
  };

  /// Payload for `update()`: same as [toInsertMap] — `id` is used as the
  /// filter, not a column to write, and `updated_at` is maintained by the
  /// `activities_set_updated_at` trigger rather than the client's clock.
  Map<String, dynamic> toUpdateMap() => toInsertMap();

  /// Value equality over every field that matters for *what this activity
  /// actually is* — deliberately excludes [createdAt]/[updatedAt] (server-
  /// maintained timestamps that can legitimately differ between two fetches
  /// of an otherwise-unchanged row, e.g. re-parsed `DateTime` instances)
  /// since comparing those would make refresh-triggered reconfiguration
  /// (see `RobotHomeScreen`) fire on meaningless timestamp jitter instead
  /// of on a real content change.
  ///
  /// This is what lets a Web-side edit — interaction mode, prompt,
  /// participants, type, enabled — be detected as "this is a different
  /// activity now" even though [id] is unchanged, so a stale in-memory
  /// [Activity] (and the [VoiceSessionController] built from it) can be
  /// told apart from a merely-re-fetched-identical one.
  @override
  bool operator ==(Object other) {
    if (identical(this, other)) return true;
    return other is Activity &&
        other.id == id &&
        other.title == title &&
        other.type == type &&
        other.systemPrompt == systemPrompt &&
        other.interactionMode == interactionMode &&
        listEquals(other.participants, participants) &&
        other.enabled == enabled &&
        other.sortOrder == sortOrder;
  }

  @override
  int get hashCode => Object.hash(
    id,
    title,
    type,
    systemPrompt,
    interactionMode,
    Object.hashAll(participants),
    enabled,
    sortOrder,
  );

  Activity copyWith({
    String? id,
    String? title,
    ActivityType? type,
    String? systemPrompt,
    InteractionMode? interactionMode,
    List<String>? participants,
    bool? enabled,
    int? sortOrder,
    DateTime? createdAt,
    DateTime? updatedAt,
  }) {
    return Activity(
      id: id ?? this.id,
      title: title ?? this.title,
      type: type ?? this.type,
      systemPrompt: systemPrompt ?? this.systemPrompt,
      interactionMode: interactionMode ?? this.interactionMode,
      participants: participants ?? this.participants,
      enabled: enabled ?? this.enabled,
      sortOrder: sortOrder ?? this.sortOrder,
      createdAt: createdAt ?? this.createdAt,
      updatedAt: updatedAt ?? this.updatedAt,
    );
  }
}
